#define ARENA_IMPLEMENTATION
#include "arena.h"

#include <iostream>
#include <cassert>
#include <cstring>
#include <string_view>
#include <csignal>
#include <thread>
#include <vector>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>

// ============================================================
// Constants
// ============================================================

constexpr int              PORT = 8080;
constexpr int              MAX_CONNECTIONS = 4096;
constexpr int              BUFFER_SIZE = 4096;
constexpr int              SQ_IDLE_MS = 500;
constexpr u64              ACCEPT_TASK_ID = 0xFFFFFFFFFFFFFFFF;
constexpr std::string_view HTTP_RESPONSE =
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/plain\r\n"
"Connection: keep-alive\r\n"
"Content-Length: 13\r\n"
"\r\n"
"Hello, World!";

static volatile std::sig_atomic_t g_shutdown = 0;

// ============================================================
// RequestContext
// ============================================================

enum class EventType { READ, WRITE, CANCEL };
enum class ConnState { ACTIVE, CLOSING };

struct RequestContext
{
  EventType       type;
  ConnState       state = ConnState::ACTIVE;
  int             client_fd = -1;
  size_t          bytes_done = 0;
  size_t          bytes_total = 0;
  char            buffer[BUFFER_SIZE];
  RequestContext* next_free = nullptr;

  void reset()
  {
    state = ConnState::ACTIVE;
    bytes_done = 0;
    bytes_total = 0;
    client_fd = -1;
  }
};

// ============================================================
// ConnectionPool
// ============================================================

class ConnectionPool
{
public:
  explicit ConnectionPool(Arena* arena)
  {
    assert(arena != nullptr && "ConnectionPool requires a valid arena");
    RequestContext* ctxs = push_array_no_zero(arena, RequestContext, MAX_CONNECTIONS);
    assert(ctxs != nullptr && "Arena failed to allocate connection contexts");
    for (int i = 0; i < MAX_CONNECTIONS - 1; ++i)
      ctxs[i].next_free = &ctxs[i + 1];
    ctxs[MAX_CONNECTIONS - 1].next_free = nullptr;
    m_head = &ctxs[0];
  }

  ConnectionPool(const ConnectionPool&) = delete;
  ConnectionPool& operator=(const ConnectionPool&) = delete;

  RequestContext* acquire()
  {
    if (!m_head) { std::cerr << "Warning: connection pool exhausted\n"; return nullptr; }
    RequestContext* ctx = m_head;
    m_head = ctx->next_free;
    ctx->reset();
    assert(ctx->client_fd == -1 && "Acquired context has stale fd");
    assert(ctx->bytes_done == 0 && "Acquired context has stale byte count");
    return ctx;
  }

  void release(RequestContext* ctx)
  {
    assert(ctx != nullptr && "Releasing a null context");
    if (ctx->client_fd >= 0) { close(ctx->client_fd); ctx->client_fd = -1; }
    ctx->next_free = m_head;
    m_head = ctx;
  }

private:
  RequestContext* m_head = nullptr;
};

// ============================================================
// ServerSocket  —  SO_REUSEPORT lets each thread own one fd
// ============================================================

class ServerSocket
{
public:
  explicit ServerSocket(int port)
  {
    assert(port > 0 && port <= 65535 && "Invalid port number");

    m_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_fd < 0) { perror("socket"); return; }

    int opt = 1;
    setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // Each worker thread binds its own fd to the same port.
    // Kernel load-balances incoming connections across all fds.
    setsockopt(m_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind");   return; }
    if (listen(m_fd, SOMAXCONN) < 0) { perror("listen"); return; }
    m_ok = true;
  }

  ~ServerSocket() { if (m_fd >= 0) close(m_fd); }

  ServerSocket(const ServerSocket&) = delete;
  ServerSocket& operator=(const ServerSocket&) = delete;
  ServerSocket(ServerSocket&& o) noexcept : m_fd(o.m_fd), m_ok(o.m_ok) { o.m_fd = -1; o.m_ok = false; }

  bool ok() const { return m_ok; }
  int  fd() const { assert(m_ok && "Accessing fd of invalid ServerSocket"); return m_fd; }

private:
  int  m_fd = -1;
  bool m_ok = false;
};

// ============================================================
// Ring
// ============================================================

struct Ring
{
  explicit Ring(int queue_depth, bool use_sqpoll = false)
  {
    if (use_sqpoll)
    {
      io_uring_params params{};
      params.flags = IORING_SETUP_SQPOLL;
      params.sq_thread_idle = SQ_IDLE_MS;
      if (io_uring_queue_init_params(queue_depth, &m_ring, &params) == 0)
      {
        m_ok = true; return;
      }
      std::cerr << "SQPOLL init failed, falling back\n";
    }
    if (io_uring_queue_init(queue_depth, &m_ring, 0) < 0)
    {
      std::cerr << "io_uring init failed\n"; return;
    }
    m_ok = true;
  }

  // ~Ring() { if (m_ok) io_uring_queue_exit(&m_ring); }

  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  bool ok()  const { return m_ok; }
  enum class WaitResult { OK, TIMEOUT, ERROR };

  WaitResult wait(io_uring_cqe** cqe)
  {
    __kernel_timespec ts{ .tv_sec = 0, .tv_nsec = 100'000'000 };
    int ret = io_uring_wait_cqe_timeout(&m_ring, cqe, &ts);
    if (ret == 0)        return WaitResult::OK;
    if (ret == -ETIME)   return WaitResult::TIMEOUT;
    return WaitResult::ERROR;
  }

  void seen(io_uring_cqe* cqe)
  {
    assert(cqe != nullptr && "Marking null CQE as seen");
    io_uring_cqe_seen(&m_ring, cqe);
  }

  void submit()
  {
    assert(m_ok && "Calling submit() on invalid Ring");
    io_uring_submit(&m_ring);
  }

  void arm_multishot_accept(int server_fd)
  {
    assert(server_fd >= 0 && "Invalid server fd for multishot accept");
    io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    assert(sqe != nullptr && "SQ ring full");
    io_uring_prep_multishot_accept(sqe, server_fd, nullptr, nullptr, 0);
    io_uring_sqe_set_data64(sqe, ACCEPT_TASK_ID);
  }

  void add_read(RequestContext* ctx)
  {
    assert(ctx != nullptr && "Null context passed to add_read");
    assert(ctx->client_fd >= 0 && "add_read called with invalid fd");
    assert(ctx->state == ConnState::ACTIVE && "add_read called on closing context");
    ctx->type = EventType::READ;
    ctx->bytes_done = 0;
    io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    assert(sqe != nullptr && "SQ ring full");
    io_uring_prep_recv(sqe, ctx->client_fd, ctx->buffer, BUFFER_SIZE, 0);
    io_uring_sqe_set_data(sqe, ctx);
  }

  void resume_read(RequestContext* ctx)
  {
    assert(ctx != nullptr && "Null context passed to resume_read");
    assert(ctx->type == EventType::READ && "resume_read called on non-READ context");
    assert(ctx->bytes_done < (size_t)BUFFER_SIZE && "resume_read called with full buffer");
    io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    assert(sqe != nullptr && "SQ ring full");
    io_uring_prep_recv(sqe, ctx->client_fd,
      ctx->buffer + ctx->bytes_done,
      BUFFER_SIZE - ctx->bytes_done, 0);
    io_uring_sqe_set_data(sqe, ctx);
  }

  void add_write(RequestContext* ctx, size_t offset = 0)
  {
    assert(ctx != nullptr && "Null context passed to add_write");
    assert(ctx->client_fd >= 0 && "add_write called with invalid fd");
    assert(offset < HTTP_RESPONSE.size() && "Write offset exceeds response size");
    assert(ctx->state == ConnState::ACTIVE && "add_write called on closing context");
    ctx->type = EventType::WRITE;
    ctx->bytes_total = HTTP_RESPONSE.size();
    io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    assert(sqe != nullptr && "SQ ring full");
    io_uring_prep_send(sqe, ctx->client_fd,
      HTTP_RESPONSE.data() + offset,
      HTTP_RESPONSE.size() - offset, MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, ctx);
  }

  void begin_close(RequestContext* ctx)
  {
    assert(ctx != nullptr && "Null context passed to begin_close");
    assert(ctx->client_fd >= 0 && "begin_close called with invalid fd");
    if (ctx->state == ConnState::CLOSING) return;
    ctx->state = ConnState::CLOSING;
    ctx->type = EventType::CANCEL;
    io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    assert(sqe != nullptr && "SQ ring full");
    io_uring_prep_cancel_fd(sqe, ctx->client_fd, IORING_ASYNC_CANCEL_ALL);
    io_uring_sqe_set_data(sqe, ctx);
  }

  io_uring m_ring{};
  bool     m_ok = false;
};

// ============================================================
// Server  —  one full instance per thread
// ============================================================

class Server
{
public:
  Server()
    : m_arena(arena_alloc(GB(1), MB(1), 0))
    , m_pool(m_arena)
    , m_socket(PORT)
    , m_ring(MAX_CONNECTIONS * 2)
  {
    assert(m_arena != nullptr && "Arena allocation failed");
  }

  // ~Server() { arena_release(m_arena); }

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool ok() const { return m_arena && m_socket.ok() && m_ring.ok(); }

  void run()
  {
    assert(ok() && "run() called on invalid Server");
    std::cout << "Worker thread " << pthread_self() << " listening on port " << PORT << "\n";

    m_ring.arm_multishot_accept(m_socket.fd());
    m_ring.submit();

    while (!g_shutdown)
    {
      io_uring_cqe* cqe;
      auto wait_result = m_ring.wait(&cqe);
      if (wait_result == Ring::WaitResult::TIMEOUT) continue;  // just check g_shutdown
      if (wait_result == Ring::WaitResult::ERROR)
      {
        std::cerr << "wait_cqe error\n";
        break;
      }

      u64          user_data = io_uring_cqe_get_data64(cqe);
      int          result = cqe->res;
      unsigned int flags = cqe->flags;
      m_ring.seen(cqe);

      if (user_data == ACCEPT_TASK_ID)
        handle_accept(result, flags);
      else
        handle_completion(reinterpret_cast<RequestContext*>(user_data), result);

      m_ring.submit();
    }
    drain();  // finish in-flight, then return
  }

  void drain()
  {
    // 1. Cancel all pending client ops
    // This generates CQEs for every in-flight recv/send
    io_uring_sqe* sqe = io_uring_get_sqe(&m_ring.m_ring);
    io_uring_prep_cancel_fd(sqe, m_socket.fd(), IORING_ASYNC_CANCEL_ALL);
    io_uring_sqe_set_data64(sqe, 0);
    m_ring.submit();

    // 2. Drain until no more CQEs
    io_uring_cqe* cqe;
    while (io_uring_peek_cqe(&m_ring.m_ring, &cqe) == 0)
    {
      if (!cqe) break;
      u64 user_data = io_uring_cqe_get_data64(cqe);
      io_uring_cqe_seen(&m_ring.m_ring, cqe);

      if (user_data != 0 && user_data != ACCEPT_TASK_ID)
      {
        RequestContext* ctx = reinterpret_cast<RequestContext*>(user_data);
        if (ctx->client_fd >= 0)
          close(ctx->client_fd);  // sends TCP FIN to client
      }
    }
  }


private:
  void handle_accept(int result, unsigned int flags)
  {
    if (result >= 0)
    {
      RequestContext* ctx = m_pool.acquire();
      if (ctx) { ctx->client_fd = result; m_ring.add_read(ctx); }
      else { close(result); }
    }
    else
    {
      std::cerr << "accept error: " << strerror(-result) << "\n";
    }
    if (!(flags & IORING_CQE_F_MORE))
      m_ring.arm_multishot_accept(m_socket.fd());
  }

  void handle_completion(RequestContext* ctx, int result)
  {
    assert(ctx != nullptr && "Null context in CQE user_data");
    switch (ctx->type)
    {
    case EventType::READ:   on_read(ctx, result); break;
    case EventType::WRITE:  on_write(ctx, result); break;
    case EventType::CANCEL: on_cancel(ctx);         break;
    }
  }

  void on_read(RequestContext* ctx, int result)
  {
    assert(ctx->type == EventType::READ && "on_read called with wrong EventType");
    if (result <= 0) { m_ring.begin_close(ctx); return; }

    ctx->bytes_done += (size_t)result;
    assert(ctx->bytes_done <= (size_t)BUFFER_SIZE && "Read overflow");

    if (!request_complete(ctx) && ctx->bytes_done < BUFFER_SIZE)
      m_ring.resume_read(ctx);
    else
    {
      ctx->bytes_done = 0;
      m_ring.add_write(ctx);
    }
  }

  void on_write(RequestContext* ctx, int result)
  {
    assert(ctx->type == EventType::WRITE && "on_write called with wrong EventType");
    if (result <= 0) { m_ring.begin_close(ctx); return; }

    ctx->bytes_done += (size_t)result;
    assert(ctx->bytes_done <= ctx->bytes_total && "Write overflow");

    if (ctx->bytes_done < ctx->bytes_total)
      m_ring.add_write(ctx, ctx->bytes_done);
    else
      m_ring.add_read(ctx);
  }

  void on_cancel(RequestContext* ctx)
  {
    assert(ctx->state == ConnState::CLOSING && "on_cancel called on non-closing context");
    m_pool.release(ctx);
  }

  static bool request_complete(const RequestContext* ctx)
  {
    assert(ctx->bytes_done > 0 && "request_complete called with no data");
    return memmem(ctx->buffer, ctx->bytes_done, "\r\n\r\n", 4) != nullptr;
  }

  Arena* m_arena = nullptr;
  ConnectionPool m_pool;
  ServerSocket   m_socket;
  Ring           m_ring;
};

// ============================================================
// Worker thread entry
// ============================================================

void run_worker(int core_id)
{
  // Block SIGINT — let main thread receive it, then broadcast via pthread_kill
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  pthread_sigmask(SIG_BLOCK, &mask, nullptr);


  // Pin thread to a specific core to avoid scheduler migration
  // and keep the ring's memory local to one CPU cache
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

  Server server;
  if (!server.ok())
  {
    std::cerr << "Worker " << core_id << " failed to init\n";
    return;
  }
  server.run();
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[])
{
  // bool use_sqpoll = (argc > 1 && std::string_view(argv[1]) == "--sqpoll");
  signal(SIGINT, [](int) { g_shutdown = 1; });

  int num_threads = (int)std::thread::hardware_concurrency();
  std::cout << "Spawning " << num_threads << " worker threads\n";

  std::vector<std::thread> workers;
  workers.reserve(num_threads);

  for (int i = 0; i < num_threads; ++i)
    workers.emplace_back(run_worker, i);

  std::vector<pthread_t> thread_ids;
  for (auto& t : workers)
    thread_ids.push_back(t.native_handle());
  while (!g_shutdown) pause();

  for (pthread_t tid : thread_ids)
    pthread_kill(tid, SIGINT);


  for (auto& t : workers)
  {
    if (t.joinable())
    {
      t.join();
    }
  }

  return 0;
}
