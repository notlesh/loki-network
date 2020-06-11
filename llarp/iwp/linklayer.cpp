#include <iwp/linklayer.hpp>
#include <iwp/session.hpp>
#include <config/key_manager.hpp>
#include <memory>
#include <unordered_set>

namespace llarp
{
  namespace iwp
  {
    LinkLayer::LinkLayer(
        std::shared_ptr<KeyManager> keyManager,
        GetRCFunc getrc,
        LinkMessageHandler h,
        SignBufferFunc sign,
        SessionEstablishedHandler est,
        SessionRenegotiateHandler reneg,
        TimeoutHandler timeout,
        SessionClosedHandler closed,
        PumpDoneHandler pumpDone,
        bool allowInbound)
        : ILinkLayer(keyManager, getrc, h, sign, est, reneg, timeout, closed, pumpDone)
        , permitInbound{allowInbound}
    {
    }

    LinkLayer::~LinkLayer() = default;

    const char*
    LinkLayer::Name() const
    {
      return "iwp";
    }

    uint16_t
    LinkLayer::Rank() const
    {
      return 2;
    }

    void
    LinkLayer::QueueWork(std::function<void(void)> func)
    {
      m_Worker->addJob(func);
    }

    void
    LinkLayer::RecvFrom(const SockAddr& from, ILinkSession::Packet_t pkt)
    {
      std::shared_ptr<ILinkSession> session;
      auto itr = m_AuthedAddrs.find(from);
      bool isNewSession = false;
      if (itr == m_AuthedAddrs.end())
      {
        Lock_t lock(m_PendingMutex);
        if (m_Pending.count(from) == 0)
        {
          if (not permitInbound)
            return;
          isNewSession = true;
          m_Pending.insert({from, std::make_shared<Session>(this, from)});
        }
        session = m_Pending.find(from)->second;
      }
      else
      {
        Lock_t lock(m_AuthedLinksMutex);
        auto range = m_AuthedLinks.equal_range(itr->second);
        session = range.first->second;
      }
      if (session)
      {
        bool success = session->Recv_LL(std::move(pkt));
        if (!success and isNewSession)
        {
          LogWarn("Brand new session failed; removing from pending sessions list");
          m_Pending.erase(m_Pending.find(from));
        }
      }
    }

    bool
    LinkLayer::MapAddr(const RouterID& r, ILinkSession* s)
    {
      if (!ILinkLayer::MapAddr(r, s))
        return false;
      m_AuthedAddrs.emplace(s->GetRemoteEndpoint(), r);
      return true;
    }

    void
    LinkLayer::UnmapAddr(const IpAddress& addr)
    {
      m_AuthedAddrs.erase(addr);
    }

    std::shared_ptr<ILinkSession>
    LinkLayer::NewOutboundSession(const RouterContact& rc, const AddressInfo& ai)
    {
      std::cout << "MAKING NEW OUTBOUND SESSION" << std::endl;
      return std::make_shared<Session>(this, rc, ai);
    }
  }  // namespace iwp
}  // namespace llarp
