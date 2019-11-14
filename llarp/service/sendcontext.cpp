#include <service/sendcontext.hpp>

#include <router/abstractrouter.hpp>
#include <routing/path_transfer_message.hpp>
#include <service/endpoint.hpp>
#include <util/thread/logic.hpp>
#include <utility>
#include <unordered_set>

namespace llarp
{
  namespace service
  {
    SendContext::SendContext(ServiceInfo ident, const Introduction& intro,
                             path::PathSet* send, Endpoint* ep)
        : remoteIdent(std::move(ident))
        , remoteIntro(intro)
        , m_PathSet(send)
        , m_DataHandler(ep)
        , m_Endpoint(ep)
    {
      createdAt = ep->Now();
    }

    bool
    SendContext::Send(std::shared_ptr< ProtocolFrame > msg, path::Path_ptr path)
    {
      util::Lock lock(&m_SendQueueMutex);
      m_SendQueue.emplace_back(
          std::make_shared< const routing::PathTransferMessage >(
              *msg, remoteIntro.pathID),
          path);
      return true;
    }

    void
    SendContext::FlushUpstream()
    {
      auto r = m_Endpoint->Router();
      std::unordered_set< path::Path_ptr, path::Path::Ptr_Hash > flushpaths;
      {
        util::Lock lock(&m_SendQueueMutex);
        for(const auto& item : m_SendQueue)
        {
          if(item.second->SendRoutingMessage(*item.first, r))
          {
            lastGoodSend = r->Now();
            flushpaths.emplace(item.second);
            m_Endpoint->MarkConvoTagActive(item.first->T.T);
          }
        }
        m_SendQueue.clear();
      }
      // flush the select path's upstream
      for(const auto& path : flushpaths)
      {
        path->FlushUpstream(r);
      }
    }

    /// send on an established convo tag
    void
    SendContext::EncryptAndSendTo(const llarp_buffer_t& payload, ProtocolType t)
    {
      SharedSecret shared;
      auto f = std::make_shared< ProtocolFrame >();
      f->N.Randomize();
      f->T = currentConvoTag;
      f->S = ++sequenceNo;

      auto path = m_PathSet->GetNewestPathByRouter(remoteIntro.router);
      if(!path)
      {
        LogError(m_Endpoint->Name(),
                 " cannot encrypt and send: no path for intro ", remoteIntro);
        return;
      }

      if(!m_DataHandler->GetCachedSessionKeyFor(f->T, shared))
      {
        LogError(m_Endpoint->Name(),
                 " has no cached session key on session T=", f->T);
        return;
      }

      auto m = std::make_shared< ProtocolMessage >();
      m_DataHandler->PutIntroFor(f->T, remoteIntro);
      m_DataHandler->PutReplyIntroFor(f->T, path->intro);
      m->proto      = t;
      m->seqno      = m_Endpoint->GetSeqNoForConvo(f->T);
      m->introReply = path->intro;
      f->F          = m->introReply.pathID;
      m->sender     = m_Endpoint->GetIdentity().pub;
      m->tag        = f->T;
      m->PutBuffer(payload);
      auto self = this;
      m_Endpoint->CryptoWorker()->addJob([f, m, shared, path, self]() {
        if(!f->EncryptAndSign(*m, shared, self->m_Endpoint->GetIdentity()))
        {
          LogError(self->m_Endpoint->Name(), " failed to sign message");
          return;
        }
        LogicCall(self->m_Endpoint->RouterLogic(), [self, f, path]() {
          self->Send(f, path);
          self->FlushUpstream();
        });
      });
    }

    void
    SendContext::AsyncEncryptAndSendTo(const llarp_buffer_t& data,
                                       ProtocolType protocol)
    {
      if(lastGoodSend)
      {
        EncryptAndSendTo(data, protocol);
      }
      else
      {
        AsyncGenIntro(data, protocol);
      }
    }
  }  // namespace service

}  // namespace llarp
