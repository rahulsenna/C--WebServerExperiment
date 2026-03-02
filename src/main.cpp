#define ARENA_IMPLEMENTATION
#include "arena.h"

#include <iostream>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>

constexpr int PORT = 8080;
constexpr int MAX_CONNECTIONS = 4096; // Easy to scale up with arenas
constexpr int BUFFER_SIZE = 1024;

// We use a reserved memory address/ID to identify the permanent accept task
constexpr __u64 ACCEPT_TASK_ID = 0xFFFFFFFFFFFFFFFF;

// const std::string HTTP_RESPONSE =
//     "HTTP/1.1 200 OK\r\n"
//     "Content-Type: text/plain\r\n"
//     "Connection: close\r\n"
//     "Content-Length: 13\r\n"
//     "\r\n"
//     "Hello, World!";

const std::string HTTP_RESPONSE =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 13\r\n"
    "\r\n"
    "Hello, World!"; 

enum class EventType
{
  READ,
  WRITE
};

// The context object for active connections
struct RequestContext
{
  EventType type;
  int client_fd;
  char buffer[BUFFER_SIZE];
  RequestContext *next_free; // Used to chain unused contexts together
};

// Global server state holding our free-list
struct ServerState
{
  RequestContext *free_list_head = nullptr;
};

// --- Memory Pool Helpers ---

RequestContext *get_context(ServerState &state)
{
  if (!state.free_list_head)
  {
    std::cerr << "Warning: Server out of connection contexts!\n";
    return nullptr;
  }
  RequestContext *ctx = state.free_list_head;
  state.free_list_head = ctx->next_free;
  return ctx;
}

void release_context(ServerState &state, RequestContext *ctx)
{
  ctx->next_free = state.free_list_head;
  state.free_list_head = ctx;
}

// --- io_uring Helpers ---

void arm_multishot_accept(io_uring *ring, int server_fd)
{
  io_uring_sqe *sqe = io_uring_get_sqe(ring);
  // NULLs for sockaddr since we don't care about the client IP in this sketch
  io_uring_prep_multishot_accept(sqe, server_fd, nullptr, nullptr, 0);
  io_uring_sqe_set_data64(sqe, ACCEPT_TASK_ID);
}

void add_read_request(io_uring *ring, RequestContext *ctx)
{
  io_uring_sqe *sqe = io_uring_get_sqe(ring);
  ctx->type = EventType::READ;
  io_uring_prep_read(sqe, ctx->client_fd, ctx->buffer, BUFFER_SIZE, 0);
  io_uring_sqe_set_data(sqe, ctx);
}

void add_write_request(io_uring *ring, RequestContext *ctx)
{
  io_uring_sqe *sqe = io_uring_get_sqe(ring);
  ctx->type = EventType::WRITE;
  io_uring_prep_write(sqe, ctx->client_fd, HTTP_RESPONSE.c_str(), HTTP_RESPONSE.size(), 0);
  io_uring_sqe_set_data(sqe, ctx);
}

// --- Main Server ---

int main()
{
  // 1. Initialize Arena and Free-List
  // 1GB reserve, 1MB commit - adjust based on MAX_CONNECTIONS
  Arena *arena = arena_alloc(GB(1), 1024 * 1024, 0);
  ServerState state;

  // Batch allocate all contexts at startup using the arena
  RequestContext *contexts = push_array_no_zero(arena, RequestContext, MAX_CONNECTIONS);

  // Wire them up into a linked list
  for (int i = 0; i < MAX_CONNECTIONS - 1; ++i)
  {
    contexts[i].next_free = &contexts[i + 1];
  }
  contexts[MAX_CONNECTIONS - 1].next_free = nullptr;
  state.free_list_head = &contexts[0];

  // 2. Setup server socket
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

  bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr));
  listen(server_fd, MAX_CONNECTIONS);

  // 3. Initialize io_uring
  // io_uring ring;
  // if (io_uring_queue_init(MAX_CONNECTIONS*2, &ring, 0) < 0)
  // {
  //   std::cerr << "Failed to init io_uring\n";
  //   return 1;
  // }

  io_uring ring;
  io_uring_params params{};
  params.flags = IORING_SETUP_SQPOLL;
  params.sq_thread_idle = 2000; // milliseconds kernel thread stays awake

  if (io_uring_queue_init_params(MAX_CONNECTIONS * 2, &ring, &params) < 0)
  {
    std::cerr << "Failed to init io_uring with SQPOLL\n";
    return 1;
  }

  std::cout << "Server listening on port " << PORT << " with multishot accept...\n";

  // 4. Arm the single multishot accept SQE
  arm_multishot_accept(&ring, server_fd);
  io_uring_submit(&ring);

  // 5. Event Loop
  while (true)
  {
    io_uring_cqe *cqe;
    if (io_uring_wait_cqe(&ring, &cqe) < 0)
    {
      std::cerr << "Error waiting for CQE\n";
      break;
    }

    __u64 user_data = io_uring_cqe_get_data64(cqe);
    int result = cqe->res;
    unsigned int flags = cqe->flags;

    if (user_data == ACCEPT_TASK_ID)
    {
      // It's our multishot accept task firing!
      if (result >= 0)
      {
        int client_fd = result;

        RequestContext *ctx = get_context(state);
        if (ctx)
        {
          ctx->client_fd = client_fd;
          add_read_request(&ring, ctx);
        }
        else
        {
          // Out of memory slots, drop the connection
          close(client_fd);
        }
      }
      else
      {
        std::cerr << "Accept error: " << strerror(-result) << "\n";
      }

      // CRITICAL MULTISHOT CHECK:
      // If the kernel drops the IORING_CQE_F_MORE flag, the multishot
      // operation has terminated. We must submit a new one to keep accepting.
      if (!(flags & IORING_CQE_F_MORE))
      {
        arm_multishot_accept(&ring, server_fd);
      }
    }
    else
    {
      // It's a standard READ or WRITE completion
      RequestContext *ctx = reinterpret_cast<RequestContext *>(user_data);

      if (result <= 0)
      {
        // Connection closed or error
        close(ctx->client_fd);
        release_context(state, ctx); // Return context to the arena pool
      }
      else
      {
        switch (ctx->type)
        {
        case EventType::READ:
          add_write_request(&ring, ctx);
          break;
        case EventType::WRITE:
          // shutdown(ctx->client_fd, SHUT_WR);
          // close(ctx->client_fd);
          // release_context(state, ctx);
            add_read_request(&ring, ctx); // reuse connection
          break;
        }
      }
    }

    io_uring_cqe_seen(&ring, cqe);
    io_uring_submit(&ring);
  }

  io_uring_queue_exit(&ring);
  arena_release(arena); // Clean up the arena on shutdown
  close(server_fd);
  return 0;
}