#include <dht/messages/pubintro.hpp>

#include <dht/context.hpp>
#include <dht/messages/gotintro.hpp>
#include <messages/dht_immediate.hpp>
#include <router/abstractrouter.hpp>
#include <routing/dht_message.hpp>
#include <nodedb.hpp>

#include <tooling/dht_event.hpp>

namespace llarp
{
  namespace dht
  {
    const uint64_t PublishIntroMessage::MaxPropagationDepth = 5;
    PublishIntroMessage::~PublishIntroMessage()             = default;

    bool
    PublishIntroMessage::DecodeKey(const llarp_buffer_t &key,
                                   llarp_buffer_t *val)
    {
      bool read = false;
      if(!BEncodeMaybeReadDictEntry("I", introset, read, key, val))
        return false;
      if(!BEncodeMaybeReadDictInt("O", relayOrder, read, key, val))
        return false;
      uint64_t relayedInt = (relayed ? 1 : 0);
      if(!BEncodeMaybeReadDictInt("R", relayedInt, read, key, val))
        return false;
      if(!BEncodeMaybeReadDictInt("T", txID, read, key, val))
        return false;
      if(!BEncodeMaybeReadDictInt("V", version, read, key, val))
        return false;
      return read;
    }

    bool
    PublishIntroMessage::HandleMessage(
        llarp_dht_context *ctx,
        std::vector< std::unique_ptr< IMessage > > &replies) const
    {
      const llarp::dht::Key_t addr(introset.derivedSigningKey);

      auto router = ctx->impl->GetRouter();
      auto ev = std::make_unique<tooling::PubIntroReceivedEvent>(router->pubkey(), Key_t(relayed ? router->pubkey() : From.data()), addr, txID, relayOrder);
      router->NotifyRouterEvent(std::move(ev));
      auto now = ctx->impl->Now();

      auto &dht = *ctx->impl;
      if(!introset.Verify(now))
      {
        llarp::LogWarn("Received PublishIntroMessage with invalid introset: ",
                       introset);
        // don't propogate or store
        replies.emplace_back(new GotIntroMessage({}, txID));
        return true;
      }

      if(introset.IsExpired(now + llarp::service::MAX_INTROSET_TIME_DELTA))
      {
        // don't propogate or store
        llarp::LogWarn("Received PublishIntroMessage with expired Introset: ",
                       introset);
        replies.emplace_back(new GotIntroMessage({}, txID));
        return true;
      }

      // identify closest 4 routers
      auto closestRCs = dht.GetRouter()->nodedb()->FindClosestTo(addr, 4);
      if(closestRCs.size() != 4)
      {
        llarp::LogWarn("Received PublishIntroMessage but only know ",
                       closestRCs.size(), " nodes");
        replies.emplace_back(new GotIntroMessage({}, txID));
        return true;
      }

      const auto &us = dht.OurKey();

      // function to identify the closest 4 routers we know of for this introset
      auto propagateToClosestFour = [&]() {
        // grab 1st & 2nd if we are relayOrder == 0, 3rd & 4th otherwise
        const auto &rc0 = (relayOrder == 0 ? closestRCs[0] : closestRCs[2]);
        const auto &rc1 = (relayOrder == 0 ? closestRCs[1] : closestRCs[3]);

        const Key_t peer0{rc0.pubkey};
        const Key_t peer1{rc1.pubkey};

        bool arePeer0 = (peer0 == us);
        bool arePeer1 = (peer1 == us);

        if(arePeer0 or arePeer1)
        {
          dht.services()->PutNode(introset);
          replies.emplace_back(new GotIntroMessage({introset}, txID));
        }

        if(not arePeer0)
          dht.PropagateIntroSetTo(From, txID, introset, peer0, false, 0);

        if(not arePeer1)
          dht.PropagateIntroSetTo(From, txID, introset, peer1, false, 0);
      };

      if(relayed)
      {
        if(relayOrder > 1)
        {
          llarp::LogWarn(
              "Received PublishIntroMessage with invalid relayOrder: ",
              relayOrder);
          replies.emplace_back(new GotIntroMessage({}, txID));
          return true;
        }

        propagateToClosestFour();
      }
      else
      {
        bool found = false;
        for(const auto &rc : closestRCs)
        {
          if(rc.pubkey == dht.OurKey())
          {
            found = true;
            break;
          }
        }

        if(found)
        {
          dht.services()->PutNode(introset);
          replies.emplace_back(new GotIntroMessage({introset}, txID));
        }
        else
        {
          // TODO: ensure this can't create a loop (reintroduce depth?)
          propagateToClosestFour();
        }
      }

      return true;
    }

    bool
    PublishIntroMessage::BEncode(llarp_buffer_t *buf) const
    {
      if(!bencode_start_dict(buf))
        return false;
      if(!BEncodeWriteDictMsgType(buf, "A", "I"))
        return false;
      if(!BEncodeWriteDictEntry("I", introset, buf))
        return false;
      if(!BEncodeWriteDictInt("O", relayOrder, buf))
        return false;
      if(!BEncodeWriteDictInt("R", relayed, buf))
        return false;
      if(!BEncodeWriteDictInt("T", txID, buf))
        return false;
      if(!BEncodeWriteDictInt("V", LLARP_PROTO_VERSION, buf))
        return false;
      return bencode_end(buf);
    }
  }  // namespace dht
}  // namespace llarp
