/* PLASMA CLIENT: Client library for using the plasma store and manager */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <strings.h>
#include <netinet/in.h>
#include <netdb.h>

#include "common.h"
#include "io.h"
#include "plasma.h"
#include "plasma_client.h"
#include "fling.h"
#include "uthash.h"

typedef struct {
  /** Key that uniquely identifies the  memory mapped file. In practice, we
   *  take the numerical value of the file descriptor in the object store. */
  int key;
  /** The result of mmap for this file descriptor. */
  uint8_t *pointer;
  /** Handle for the uthash table. */
  UT_hash_handle hh;
} client_mmap_table_entry;

/** Information about a connection between a Plasma Client and Plasma Store.
 *  This is used to avoid mapping the same files into memory multiple times. */
struct plasma_store_conn {
  /** File descriptor of the Unix domain socket that connects to the store. */
  int conn;
  /** Table of dlmalloc buffer files that have been memory mapped so far. */
  client_mmap_table_entry *mmap_table;
};

void plasma_send_request(int fd, int type, plasma_request *req) {
  int req_count = sizeof(plasma_request);
  write_message(fd, type, req_count, (uint8_t *) req);
}

/* If the file descriptor fd has been mmapped in this client process before,
 * return the pointer that was returned by mmap, otherwise mmap it and store the
 * pointer in a hash table. */
uint8_t *lookup_or_mmap(plasma_store_conn *conn,
                        int fd,
                        int store_fd_val,
                        int64_t map_size) {
  client_mmap_table_entry *entry;
  HASH_FIND_INT(conn->mmap_table, &store_fd_val, entry);
  if (entry) {
    close(fd);
    return entry->pointer;
  } else {
    uint8_t *result =
        mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (result == MAP_FAILED) {
      LOG_ERR("mmap failed");
      exit(-1);
    }
    close(fd);
    entry = malloc(sizeof(client_mmap_table_entry));
    entry->key = store_fd_val;
    entry->pointer = result;
    HASH_ADD_INT(conn->mmap_table, key, entry);
    return result;
  }
}

void plasma_create(plasma_store_conn *conn,
                   object_id object_id,
                   int64_t data_size,
                   uint8_t *metadata,
                   int64_t metadata_size,
                   uint8_t **data) {
  LOG_DEBUG("called plasma_create on conn %d with size %" PRId64
            " and metadata size "
            "%" PRId64,
            conn->conn, data_size, metadata_size);
  plasma_request req = {.object_id = object_id,
                        .data_size = data_size,
                        .metadata_size = metadata_size};
  plasma_send_request(conn->conn, PLASMA_CREATE, &req);
  plasma_reply reply;
  int fd = recv_fd(conn->conn, (char *) &reply, sizeof(plasma_reply));
  plasma_object *object = &reply.object;
  CHECK(object->data_size == data_size);
  CHECK(object->metadata_size == metadata_size);
  /* The metadata should come right after the data. */
  CHECK(object->metadata_offset == object->data_offset + data_size);
  *data = lookup_or_mmap(conn, fd, object->handle.store_fd,
                         object->handle.mmap_size) +
          object->data_offset;
  /* If plasma_create is being called from a transfer, then we will not copy the
   * metadata here. The metadata will be written along with the data streamed
   * from the transfer. */
  if (metadata != NULL) {
    /* Copy the metadata to the buffer. */
    memcpy(*data + object->data_size, metadata, metadata_size);
  }
}

/* This method is used to get both the data and the metadata. */
void plasma_get(plasma_store_conn *conn,
                object_id object_id,
                int64_t *size,
                uint8_t **data,
                int64_t *metadata_size,
                uint8_t **metadata) {
  plasma_request req = {.object_id = object_id};
  plasma_send_request(conn->conn, PLASMA_GET, &req);
  plasma_reply reply;
  int fd = recv_fd(conn->conn, (char *) &reply, sizeof(plasma_reply));
  plasma_object *object = &reply.object;
  *data = lookup_or_mmap(conn, fd, object->handle.store_fd,
                         object->handle.mmap_size) +
          object->data_offset;
  *size = object->data_size;
  /* If requested, return the metadata as well. */
  if (metadata != NULL) {
    *metadata = *data + object->data_size;
    *metadata_size = object->metadata_size;
  }
}

/* This method is used to query whether the plasma store contains an object. */
void plasma_contains(plasma_store_conn *conn,
                     object_id object_id,
                     int *has_object) {
  plasma_request req = {.object_id = object_id};
  plasma_send_request(conn->conn, PLASMA_CONTAINS, &req);
  plasma_reply reply;
  int r = read(conn->conn, &reply, sizeof(plasma_reply));
  CHECKM(r != -1, "read error");
  CHECKM(r != 0, "connection disconnected");
  *has_object = reply.has_object;
}

void plasma_seal(plasma_store_conn *conn, object_id object_id) {
  plasma_request req = {.object_id = object_id};
  plasma_send_request(conn->conn, PLASMA_SEAL, &req);
}

void plasma_delete(plasma_store_conn *conn, object_id object_id) {
  plasma_request req = {.object_id = object_id};
  plasma_send_request(conn->conn, PLASMA_DELETE, &req);
}

plasma_store_conn *plasma_store_connect(const char *socket_name) {
  assert(socket_name);
  /* Try to connect to the Plasma store. If unsuccessful, retry several times.
   */
  int fd = -1;
  int connected_successfully = 0;
  for (int num_attempts = 0; num_attempts < 50; ++num_attempts) {
    fd = connect_ipc_sock(socket_name);
    if (fd >= 0) {
      connected_successfully = 1;
      break;
    }
    /* Sleep for 100 milliseconds. */
    usleep(100000);
  }
  /* If we could not connect to the Plasma store, exit. */
  if (!connected_successfully) {
    LOG_ERR("could not connect to store %s", socket_name);
    exit(-1);
  }
  /* Initialize the store connection struct */
  plasma_store_conn *result = malloc(sizeof(plasma_store_conn));
  result->conn = fd;
  result->mmap_table = NULL;
  return result;
}

void plasma_store_disconnect(plasma_store_conn *conn) {
  close(conn->conn);
  free(conn);
}

#define h_addr h_addr_list[0]

int plasma_manager_connect(const char *ip_addr, int port) {
  int fd = socket(PF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERR("could not create socket");
    exit(-1);
  }

  struct hostent *manager = gethostbyname(ip_addr); /* TODO(pcm): cache this */
  if (!manager) {
    LOG_ERR("plasma manager %s not found", ip_addr);
    exit(-1);
  }

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  memcpy(&addr.sin_addr.s_addr, manager->h_addr, manager->h_length);
  addr.sin_port = htons(port);

  int r = connect(fd, (struct sockaddr *) &addr, sizeof(addr));
  if (r < 0) {
    LOG_ERR(
        "could not establish connection to manager with id %s:%d (probably ran "
        "out of ports)",
        &ip_addr[0], port);
    exit(-1);
  }
  return fd;
}

void plasma_transfer(int manager,
                     const char *addr,
                     int port,
                     object_id object_id) {
  plasma_request req = {.object_id = object_id, .port = port};
  char *end = NULL;
  for (int i = 0; i < 4; ++i) {
    req.addr[i] = strtol(end ? end : addr, &end, 10);
    /* skip the '.' */
    end += 1;
  }
  plasma_send_request(manager, PLASMA_TRANSFER, &req);
}
