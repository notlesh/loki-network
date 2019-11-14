#include <ev/ev_libuv.hpp>
#include <net/net_addr.hpp>
#include <util/thread/logic.hpp>
#include <util/thread/queue.hpp>

#include <cstring>

namespace libuv
{
  /// call a function in logic thread via a handle
  template < typename Handle, typename Func >
  void
  Call(Handle* h, Func f)
  {
    static_cast< Loop* >(h->loop->data)->Call(f);
  }

  struct glue
  {
    virtual ~glue() = default;
    virtual void
    Close() = 0;
  };

  /// tcp connection glue between llarp and libuv
  struct conn_glue : public glue
  {
    using WriteBuffer_t = std::vector< char >;

    struct WriteEvent
    {
      WriteBuffer_t data;
      uv_write_t request;

      WriteEvent() = default;

      explicit WriteEvent(WriteBuffer_t buf) : data(std::move(buf))
      {
        request.data = this;
      }

      uv_buf_t
      Buffer()
      {
        return uv_buf_init(data.data(), data.size());
      }

      uv_write_t*
      Request()
      {
        return &request;
      }
    };

    uv_tcp_t m_Handle;
    uv_connect_t m_Connect;
    uv_check_t m_Ticker;
    llarp_tcp_connecter* const m_TCP;
    llarp_tcp_acceptor* const m_Accept;
    llarp_tcp_conn m_Conn;
    llarp::Addr m_Addr;
    llarp::thread::Queue< WriteBuffer_t > m_WriteQueue;
    uv_async_t m_WriteNotify;

    conn_glue(uv_loop_t* loop, llarp_tcp_connecter* tcp, const sockaddr* addr)
        : m_TCP(tcp), m_Accept(nullptr), m_Addr(*addr), m_WriteQueue(32)
    {
      m_Connect.data = this;
      m_Handle.data  = this;
      m_TCP->impl    = this;
      uv_tcp_init(loop, &m_Handle);
      m_Ticker.data = this;
      uv_check_init(loop, &m_Ticker);
      m_Conn.close       = &ExplicitClose;
      m_Conn.write       = &ExplicitWrite;
      m_WriteNotify.data = this;
      uv_async_init(loop, &m_WriteNotify, &OnShouldWrite);
    }

    conn_glue(uv_loop_t* loop, llarp_tcp_acceptor* tcp, const sockaddr* addr)
        : m_TCP(nullptr), m_Accept(tcp), m_Addr(*addr), m_WriteQueue(32)
    {
      m_Connect.data = nullptr;
      m_Handle.data  = this;
      uv_tcp_init(loop, &m_Handle);
      m_Ticker.data = this;
      uv_check_init(loop, &m_Ticker);
      m_Accept->close    = &ExplicitCloseAccept;
      m_Conn.write       = nullptr;
      m_Conn.closed      = nullptr;
      m_WriteNotify.data = this;
      uv_async_init(loop, &m_WriteNotify, &OnShouldWrite);
    }

    conn_glue(conn_glue* parent)
        : m_TCP(nullptr), m_Accept(nullptr), m_WriteQueue(32)
    {
      m_Connect.data = nullptr;
      m_Conn.close   = &ExplicitClose;
      m_Conn.write   = &ExplicitWrite;
      m_Handle.data  = this;
      uv_tcp_init(parent->m_Handle.loop, &m_Handle);
      m_Ticker.data = this;
      uv_check_init(parent->m_Handle.loop, &m_Ticker);
      m_WriteNotify.data = this;
      uv_async_init(parent->m_Handle.loop, &m_WriteNotify, &OnShouldWrite);
    }

    static void
    OnOutboundConnect(uv_connect_t* c, int status)
    {
      conn_glue* self = static_cast< conn_glue* >(c->data);
      Call(self->Stream(),
           std::bind(&conn_glue::HandleConnectResult, self, status));
      c->data = nullptr;
    }

    bool
    ConnectAsync()
    {
      return uv_tcp_connect(&m_Connect, &m_Handle, m_Addr, &OnOutboundConnect)
          != -1;
    }

    static void
    ExplicitClose(llarp_tcp_conn* conn)
    {
      static_cast< conn_glue* >(conn->impl)->Close();
    }
    static void
    ExplicitCloseAccept(llarp_tcp_acceptor* tcp)
    {
      static_cast< conn_glue* >(tcp->impl)->Close();
    }

    static ssize_t
    ExplicitWrite(llarp_tcp_conn* conn, const byte_t* ptr, size_t sz)
    {
      return static_cast< conn_glue* >(conn->impl)->WriteAsync((char*)ptr, sz);
    }

    static void
    OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
    {
      if(nread >= 0)
      {
        auto* conn = static_cast< conn_glue* >(stream->data);
        Call(stream, std::bind(&conn_glue::Read, conn, buf->base, nread));
        return;
      }
      else if(nread < 0)
      {
        static_cast< conn_glue* >(stream->data)->Close();
      }
      delete[] buf->base;
    }

    static void
    Alloc(uv_handle_t*, size_t suggested_size, uv_buf_t* buf)
    {
      buf->base = new char[suggested_size];
      buf->len  = suggested_size;
    }

    void
    Read(const char* ptr, ssize_t sz)
    {
      if(m_Conn.read)
      {
        llarp::LogDebug("tcp read ", sz, " bytes");
        const llarp_buffer_t buf(ptr, sz);
        m_Conn.read(&m_Conn, buf);
      }
      delete[] ptr;
    }

    void
    HandleConnectResult(int status)
    {
      if(m_TCP && m_TCP->connected)
      {
        if(status == 0)
        {
          m_Conn.impl  = this;
          m_Conn.loop  = m_TCP->loop;
          m_Conn.close = &ExplicitClose;
          m_Conn.write = &ExplicitWrite;
          m_TCP->connected(m_TCP, &m_Conn);
          Start();
        }
        else if(m_TCP->error)
        {
          llarp::LogError("failed to connect tcp ", uv_strerror(status));
          m_TCP->error(m_TCP);
        }
      }
    }

    void
    WriteFail()
    {
      if(m_Conn.close)
        m_Conn.close(&m_Conn);
    }

    uv_stream_t*
    Stream()
    {
      return (uv_stream_t*)&m_Handle;
    }

    static void
    OnWritten(uv_write_t* req, int status)
    {
      WriteEvent* ev = static_cast< WriteEvent* >(req->data);
      if(status == 0)
      {
        llarp::LogDebug("wrote ", ev->data.size());
      }
      else
      {
        llarp::LogDebug("write fail");
      }
      delete ev;
    }

    int
    WriteAsync(char* data, size_t sz)
    {
      if(uv_is_closing((const uv_handle_t*)&m_Handle))
        return -1;
      if(uv_is_closing((const uv_handle_t*)&m_WriteNotify))
        return -1;
      WriteBuffer_t buf(sz);
      std::copy_n(data, sz, buf.begin());
      if(m_WriteQueue.pushBack(std::move(buf))
         == llarp::thread::QueueReturn::Success)
      {
        uv_async_send(&m_WriteNotify);
        return sz;
      }
      return -1;
    }

    void
    FlushWrite()
    {
      while(not m_WriteQueue.empty())
      {
        auto data      = m_WriteQueue.popFront();
        WriteEvent* ev = new WriteEvent(std::move(data));
        auto buf       = ev->Buffer();
        if(uv_write(ev->Request(), Stream(), &buf, 1, &OnWritten) != 0)
        {
          Close();
          return;
        }
      }
    }

    static void
    OnClosed(uv_handle_t* h)
    {
      conn_glue* conn = static_cast< conn_glue* >(h->data);
      Call(h, std::bind(&conn_glue::HandleClosed, conn));
    }

    static void
    FullClose(uv_handle_t* h)
    {
      auto* self = static_cast< conn_glue* >(h->data);
      h->data    = nullptr;
      delete self;
      llarp::LogDebug("deleted");
    }

    void
    HandleClosed()
    {
      m_Handle.data = nullptr;
      if(m_Accept)
      {
        if(m_Accept->closed)
          m_Accept->closed(m_Accept);
        m_Accept->impl = nullptr;
      }
      if(m_Conn.closed)
      {
        m_Conn.closed(&m_Conn);
      }
      m_Conn.impl = nullptr;
      llarp::LogDebug("closed");
      uv_close((uv_handle_t*)&m_Ticker, &FullClose);
    }

    static void
    OnShutdown(uv_shutdown_t* shut, int code)
    {
      llarp::LogDebug("shut down ", code);
      auto* self = static_cast< conn_glue* >(shut->data);
      uv_close((uv_handle_t*)&self->m_Handle, &OnClosed);
      delete shut;
    }

    static void
    OnWriteClosed(uv_handle_t* h)
    {
      conn_glue* conn = static_cast< conn_glue* >(h->data);
      auto* shut      = new uv_shutdown_t();
      shut->data      = conn;
      uv_shutdown(shut, conn->Stream(), &OnShutdown);
    }

    void
    Close() override
    {
      if(uv_is_closing((uv_handle_t*)&m_WriteNotify))
        return;
      llarp::LogDebug("close tcp connection");
      m_WriteQueue.disable();
      uv_close((uv_handle_t*)&m_WriteNotify, &OnWriteClosed);
      uv_check_stop(&m_Ticker);
      uv_read_stop(Stream());
    }

    static void
    OnAccept(uv_stream_t* stream, int status)
    {
      if(status == 0)
      {
        conn_glue* conn = static_cast< conn_glue* >(stream->data);
        Call(stream, std::bind(&conn_glue::Accept, conn));
      }
      else
      {
        llarp::LogError("tcp accept failed: ", uv_strerror(status));
      }
    }

    static void
    OnShouldWrite(uv_async_t* h)
    {
      static_cast< conn_glue* >(h->data)->FlushWrite();
    }

    static void
    OnTick(uv_check_t* t)
    {
      conn_glue* conn = static_cast< conn_glue* >(t->data);
      Call(t, std::bind(&conn_glue::Tick, conn));
    }

    void
    Tick()
    {
      if(m_Accept && m_Accept->tick)
        m_Accept->tick(m_Accept);
      if(m_Conn.tick)
        m_Conn.tick(&m_Conn);
    }

    void
    Start()
    {
      auto result = uv_check_start(&m_Ticker, &OnTick);
      if(result)
        llarp::LogError("failed to start timer ", uv_strerror(result));
      result = uv_read_start(Stream(), &Alloc, &OnRead);
      if(result)
        llarp::LogError("failed to start reader ", uv_strerror(result));
    }

    void
    Accept()
    {
      if(m_Accept && m_Accept->accepted)
      {
        auto* child = new conn_glue(this);
        llarp::LogDebug("accepted new connection");
        child->m_Conn.impl  = child;
        child->m_Conn.loop  = m_Accept->loop;
        child->m_Conn.close = &ExplicitClose;
        child->m_Conn.write = &ExplicitWrite;
        auto res            = uv_accept(Stream(), child->Stream());
        if(res)
        {
          llarp::LogError("failed to accept tcp connection ", uv_strerror(res));
          child->Close();
          return;
        }
        m_Accept->accepted(m_Accept, &child->m_Conn);
        child->Start();
      }
    }

    bool
    Server()
    {
      m_Accept->close = &ExplicitCloseAccept;
      return uv_tcp_bind(&m_Handle, m_Addr, 0) == 0
          && uv_listen(Stream(), 5, &OnAccept) == 0;
    }
  };

  struct ticker_glue : public glue
  {
    std::function< void(void) > func;

    ticker_glue(uv_loop_t* loop, std::function< void(void) > tick) : func(tick)
    {
      m_Ticker.data = this;
      uv_check_init(loop, &m_Ticker);
    }

    static void
    OnTick(uv_check_t* t)
    {
      ticker_glue* ticker = static_cast< ticker_glue* >(t->data);
      Call(t, ticker->func);
    }

    bool
    Start()
    {
      return uv_check_start(&m_Ticker, &OnTick) != -1;
    }

    void
    Close() override
    {
      uv_check_stop(&m_Ticker);
      m_Ticker.data = nullptr;
      delete this;
    }

    uv_check_t m_Ticker;
  };

  struct udp_glue : public glue
  {
    uv_udp_t m_Handle;
    uv_check_t m_Ticker;
    llarp_udp_io* const m_UDP;
    llarp::Addr m_Addr;
    llarp_pkt_list m_LastPackets;
    std::array< char, 1500 > m_Buffer;

    udp_glue(uv_loop_t* loop, llarp_udp_io* udp, const sockaddr* src)
        : m_UDP(udp), m_Addr(*src)
    {
      m_Handle.data = this;
      m_Ticker.data = this;
      uv_udp_init(loop, &m_Handle);
      uv_check_init(loop, &m_Ticker);
    }

    static void
    Alloc(uv_handle_t*, size_t suggested_size, uv_buf_t* buf)
    {
      const size_t sz = std::min(suggested_size, size_t{1500});
      buf->base       = new char[sz];
      buf->len        = sz;
    }

    static void
    OnRecv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
           const struct sockaddr* addr, unsigned)
    {
      udp_glue* glue = static_cast< udp_glue* >(handle->data);
      if(addr)
        glue->RecvFrom(nread, buf, addr);
      if(nread <= 0 || glue->m_UDP == nullptr
         || glue->m_UDP->recvfrom != nullptr)
        delete[] buf->base;
    }

    bool
    RecvMany(llarp_pkt_list* pkts)
    {
      *pkts         = std::move(m_LastPackets);
      m_LastPackets = llarp_pkt_list();
      return pkts->size() > 0;
    }

    void
    RecvFrom(ssize_t sz, const uv_buf_t* buf, const struct sockaddr* fromaddr)
    {
      if(sz > 0 && m_UDP)
      {
        const size_t pktsz = sz;
        if(m_UDP->recvfrom)
        {
          const llarp_buffer_t pkt((const byte_t*)buf->base, pktsz);
          m_UDP->recvfrom(m_UDP, fromaddr, ManagedBuffer{pkt});
        }
        else
        {
          PacketBuffer pbuf(buf->base, pktsz);
          m_LastPackets.emplace_back(PacketEvent{*fromaddr, std::move(pbuf)});
        }
      }
    }

    static void
    OnTick(uv_check_t* t)
    {
      udp_glue* udp = static_cast< udp_glue* >(t->data);
      udp->Tick();
    }

    void
    Tick()
    {
      if(m_UDP && m_UDP->tick)
        m_UDP->tick(m_UDP);
    }

    static int
    SendTo(llarp_udp_io* udp, const sockaddr* to, const byte_t* ptr, size_t sz)
    {
      auto* self = static_cast< udp_glue* >(udp->impl);
      if(self == nullptr)
        return -1;
      uv_buf_t buf = uv_buf_init((char*)ptr, sz);
      return uv_udp_try_send(&self->m_Handle, &buf, 1, to);
    }

    bool
    Bind()
    {
      auto ret = uv_udp_bind(&m_Handle, m_Addr, 0);
      if(ret)
      {
        llarp::LogError("failed to bind to ", m_Addr, " ", uv_strerror(ret));
        return false;
      }
      if(uv_udp_recv_start(&m_Handle, &Alloc, &OnRecv))
      {
        llarp::LogError("failed to start recving packets via ", m_Addr);
        return false;
      }
      if(uv_check_start(&m_Ticker, &OnTick))
      {
        llarp::LogError("failed to start ticker");
        return false;
      }
      if(uv_fileno((const uv_handle_t*)&m_Handle, &m_UDP->fd))
        return false;
      m_UDP->sendto = &SendTo;
      m_UDP->impl   = this;
      return true;
    }

    static void
    OnClosed(uv_handle_t* h)
    {
      auto* glue = static_cast< udp_glue* >(h->data);
      if(glue)
      {
        h->data = nullptr;
        delete glue;
      }
    }

    void
    Close() override
    {
      m_UDP->impl = nullptr;
      uv_check_stop(&m_Ticker);
      uv_close((uv_handle_t*)&m_Handle, &OnClosed);
    }
  };

  struct pipe_glue : public glue
  {
    byte_t m_Buffer[1024 * 8];
    llarp_ev_pkt_pipe* const m_Pipe;
    pipe_glue(uv_loop_t* loop, llarp_ev_pkt_pipe* pipe) : m_Pipe(pipe)
    {
      m_Handle.data = this;
      m_Ticker.data = this;
      uv_poll_init(loop, &m_Handle, m_Pipe->fd);
      uv_check_init(loop, &m_Ticker);
    }

    void
    Tick()
    {
      Call(&m_Handle, std::bind(&llarp_ev_pkt_pipe::tick, m_Pipe));
    }

    static void
    OnRead(uv_poll_t* handle, int status, int)
    {
      if(status)
      {
        return;
      }
      pipe_glue* glue = static_cast< pipe_glue* >(handle->data);
      int r = glue->m_Pipe->read(glue->m_Buffer, sizeof(glue->m_Buffer));
      if(r <= 0)
        return;
      const llarp_buffer_t buf{glue->m_Buffer, static_cast< size_t >(r)};
      glue->m_Pipe->OnRead(buf);
    }

    static void
    OnClosed(uv_handle_t* h)
    {
      auto* self = static_cast< pipe_glue* >(h->data);
      if(self)
      {
        h->data = nullptr;
        delete self;
      }
    }

    void
    Close() override
    {
      uv_check_stop(&m_Ticker);
      uv_close((uv_handle_t*)&m_Handle, &OnClosed);
    }

    static void
    OnTick(uv_check_t* h)
    {
      pipe_glue* pipe = static_cast< pipe_glue* >(h->data);
      Call(h, std::bind(&pipe_glue::Tick, pipe));
    }

    bool
    Start()
    {
      if(uv_poll_start(&m_Handle, UV_READABLE, &OnRead))
        return false;
      if(uv_check_start(&m_Ticker, &OnTick))
        return false;
      return true;
    }

    uv_poll_t m_Handle;
    uv_check_t m_Ticker;
  };

  struct tun_glue : public glue
  {
    uv_poll_t m_Handle;
    uv_check_t m_Ticker;
    llarp_tun_io* const m_Tun;
    device* const m_Device;
    byte_t m_Buffer[1500];
    bool readpkt;

    tun_glue(llarp_tun_io* tun) : m_Tun(tun), m_Device(tuntap_init())
    {
      m_Handle.data = this;
      m_Ticker.data = this;
      readpkt       = false;
    }

    ~tun_glue() override
    {
      tuntap_destroy(m_Device);
    }

    static void
    OnTick(uv_check_t* timer)
    {
      tun_glue* tun = static_cast< tun_glue* >(timer->data);
      Call(timer, std::bind(&tun_glue::Tick, tun));
    }

    static void
    OnPoll(uv_poll_t* h, int, int events)
    {
      if(events & UV_READABLE)
      {
        static_cast< tun_glue* >(h->data)->Read();
      }
    }

    void
    Read()
    {
      auto sz = tuntap_read(m_Device, m_Buffer, sizeof(m_Buffer));
      if(sz > 0)
      {
        llarp::LogDebug("tun read ", sz);
        llarp_buffer_t pkt(m_Buffer, sz);
        if(m_Tun && m_Tun->recvpkt)
          m_Tun->recvpkt(m_Tun, pkt);
      }
    }

    void
    Tick()
    {
      if(m_Tun->before_write)
        m_Tun->before_write(m_Tun);
      if(m_Tun->tick)
        m_Tun->tick(m_Tun);
    }

    static void
    OnClosed(uv_handle_t* h)
    {
      auto* self = static_cast< tun_glue* >(h->data);
      if(self)
      {
        h->data = nullptr;
        delete self;
      }
    }

    void
    Close() override
    {
      m_Tun->impl = nullptr;
      uv_check_stop(&m_Ticker);
      uv_close((uv_handle_t*)&m_Handle, &OnClosed);
    }

    bool
    Write(const byte_t* pkt, size_t sz)
    {
      return tuntap_write(m_Device, (void*)pkt, sz) != -1;
    }

    static bool
    WritePkt(llarp_tun_io* tun, const byte_t* pkt, size_t sz)
    {
      tun_glue* glue = static_cast< tun_glue* >(tun->impl);
      return glue && glue->Write(pkt, sz);
    }

    bool
    Init(uv_loop_t* loop)
    {
      memcpy(m_Device->if_name, m_Tun->ifname, sizeof(m_Device->if_name));
      if(tuntap_start(m_Device, TUNTAP_MODE_TUNNEL, 0) == -1)
      {
        llarp::LogError("failed to start up ", m_Tun->ifname);
        return false;
      }
      if(tuntap_set_ip(m_Device, m_Tun->ifaddr, m_Tun->ifaddr, m_Tun->netmask)
         == -1)
      {
        llarp::LogError("failed to set address on ", m_Tun->ifname);
        return false;
      }
      if(tuntap_up(m_Device) == -1)
      {
        llarp::LogError("failed to put up ", m_Tun->ifname);
        return false;
      }
      if(m_Device->tun_fd == -1)
      {
        llarp::LogError("tun interface ", m_Tun->ifname,
                        " has invalid fd: ", m_Device->tun_fd);
        return false;
      }
      if(uv_poll_init(loop, &m_Handle, m_Device->tun_fd) == -1)
      {
        llarp::LogError("failed to start polling on ", m_Tun->ifname);
        return false;
      }
      if(uv_poll_start(&m_Handle, UV_READABLE, &OnPoll))
      {
        llarp::LogError("failed to start polling on ", m_Tun->ifname);
        return false;
      }
      if(uv_check_init(loop, &m_Ticker) != 0
         || uv_check_start(&m_Ticker, &OnTick) != 0)
      {
        llarp::LogError("failed to set up tun interface timer for ",
                        m_Tun->ifname);
        return false;
      }
      m_Tun->writepkt = &WritePkt;
      m_Tun->impl     = this;
      return true;
    }
  };

  bool
  Loop::init()
  {
    if(uv_loop_init(&m_Impl) == -1)
      return false;
    m_Impl.data = this;
    uv_loop_configure(&m_Impl, UV_LOOP_BLOCK_SIGNAL, SIGPIPE);
    m_TickTimer.data = this;
    m_Run.store(true);
    return uv_timer_init(&m_Impl, &m_TickTimer) != -1;
  }

  void
  Loop::update_time()
  {
    llarp_ev_loop::update_time();
    uv_update_time(&m_Impl);
  }

  bool
  Loop::running() const
  {
    return m_Run.load();
  }

  bool
  Loop::tcp_connect(llarp_tcp_connecter* tcp, const sockaddr* addr)
  {
    auto* impl = new conn_glue(&m_Impl, tcp, addr);
    tcp->impl  = impl;
    if(impl->ConnectAsync())
      return true;
    delete impl;
    tcp->impl = nullptr;
    return false;
  }

  static void
  OnTickTimeout(uv_timer_t* timer)
  {
    uv_stop(timer->loop);
  }

  int
  Loop::tick(int ms)
  {
    uv_timer_start(&m_TickTimer, &OnTickTimeout, ms, 0);
    uv_run(&m_Impl, UV_RUN_ONCE);
    return 0;
  }

  void
  Loop::stop()
  {
    uv_stop(&m_Impl);
    llarp::LogInfo("stopping event loop");
    m_Run.store(false);
    CloseAll();
  }

  void
  Loop::CloseAll()
  {
    llarp::LogInfo("Closing all handles");
    uv_walk(
        &m_Impl,
        [](uv_handle_t* h, void*) {
          if(uv_is_closing(h))
            return;
          if(h->data && uv_is_active(h))
          {
            static_cast< glue* >(h->data)->Close();
          }
        },
        nullptr);
  }

  void
  Loop::stopped()
  {
    tick(50);
    llarp::LogInfo("we have stopped");
  }

  bool
  Loop::udp_listen(llarp_udp_io* udp, const sockaddr* src)
  {
    auto* impl = new udp_glue(&m_Impl, udp, src);
    udp->impl  = impl;
    if(impl->Bind())
    {
      return true;
    }
    delete impl;
    return false;
  }

  bool
  Loop::add_ticker(std::function< void(void) > func)
  {
    auto* ticker = new ticker_glue(&m_Impl, func);
    if(ticker->Start())
    {
      return true;
    }
    delete ticker;
    return false;
  }

  bool
  Loop::udp_close(llarp_udp_io* udp)
  {
    if(udp == nullptr)
      return false;
    auto* glue = static_cast< udp_glue* >(udp->impl);
    if(glue == nullptr)
      return false;
    glue->Close();
    return true;
  }

  bool
  Loop::tun_listen(llarp_tun_io* tun)
  {
    auto* glue = new tun_glue(tun);
    tun->impl  = glue;
    if(glue->Init(&m_Impl))
    {
      return true;
    }
    delete glue;
    return false;
  }

  bool
  Loop::tcp_listen(llarp_tcp_acceptor* tcp, const sockaddr* addr)
  {
    auto* glue = new conn_glue(&m_Impl, tcp, addr);
    tcp->impl  = glue;
    if(glue->Server())
      return true;
    tcp->impl = nullptr;
    delete glue;
    return false;
  }

  bool
  Loop::add_pipe(llarp_ev_pkt_pipe* p)
  {
    auto* glue = new pipe_glue(&m_Impl, p);
    if(glue->Start())
      return true;
    delete glue;
    return false;
  }

}  // namespace libuv

bool
llarp_ev_udp_recvmany(struct llarp_udp_io* u, struct llarp_pkt_list* pkts)
{
  return static_cast< libuv::udp_glue* >(u->impl)->RecvMany(pkts);
}