#include <dht/messages/gotintro.hpp>

#include <service/intro.hpp>
#include <dht/context.hpp>
#include <memory>
#include <path/path_context.hpp>
#include <router/abstractrouter.hpp>
#include <routing/dht_message.hpp>
#include <tooling/dht_event.hpp>
#include <utility>

namespace llarp
{
  namespace dht
  {
    GotIntroMessage::GotIntroMessage(
        std::vector< service::EncryptedIntroSet > results, uint64_t tx)
        : IMessage({}), found(std::move(results)), txid(tx)
    {
    }

    bool
    GotIntroMessage::HandleMessage(
        llarp_dht_context *ctx,
        std::vector< std::unique_ptr< IMessage > > & /*replies*/) const
    {
      auto &dht = *ctx->impl;
      auto *router = dht.GetRouter();

      auto ev = std::make_unique<tooling::GotIntroReceivedEvent>(
          router->pubkey(),
          Key_t(From.data()),
          (found.size() > 0 ? found[0] : llarp::service::EncryptedIntroSet{}),
          txid);
      router->NotifyRouterEvent(std::move(ev));

      for(const auto &introset : found)
      {
        if(!introset.Verify(dht.Now()))
        {
          LogWarn(
              "Invalid introset while handling direct GotIntro "
              "from ",
              From);
          return false;
        }
      }
      TXOwner owner(From, txid);

      auto serviceLookup =
          dht.pendingIntrosetLookups().GetPendingLookupFrom(owner);
      if(serviceLookup)
      {
        if(not found.empty())
        {
          dht.pendingIntrosetLookups().Found(owner, serviceLookup->target,
                                             found);
        }
        else
        {
          dht.pendingIntrosetLookups().NotFound(owner, nullptr);
        }
        return true;
      }
      LogError("no pending TX for GIM from ", From, " txid=", txid);
      return false;
    }

    bool
    RelayedGotIntroMessage::HandleMessage(
        llarp_dht_context *ctx,
        __attribute__((unused))
        std::vector< std::unique_ptr< IMessage > > &replies) const
    {
      // TODO: implement me better?
      auto pathset =
          ctx->impl->GetRouter()->pathContext().GetLocalPathSet(pathID);
      if(pathset)
      {
        auto copy = std::make_shared< const RelayedGotIntroMessage >(*this);
        return pathset->HandleGotIntroMessage(copy);
      }
      LogWarn("No path for got intro message pathid=", pathID);
      return false;
    }

    bool
    GotIntroMessage::DecodeKey(const llarp_buffer_t &key, llarp_buffer_t *buf)
    {
      if(key == "I")
      {
        return BEncodeReadList(found, buf);
      }
      if(key == "K")
      {
        if(closer.has_value())  // duplicate key?
          return false;
        dht::Key_t K;
        if(not K.BDecode(buf))
          return false;
        closer = K;
        return true;
      }
      bool read = false;
      if(!BEncodeMaybeReadDictInt("T", txid, read, key, buf))
        return false;
      if(!BEncodeMaybeReadDictInt("V", version, read, key, buf))
        return false;
      return read;
    }

    bool
    GotIntroMessage::BEncode(llarp_buffer_t *buf) const
    {
      if(!bencode_start_dict(buf))
        return false;
      if(!BEncodeWriteDictMsgType(buf, "A", "G"))
        return false;
      if(!BEncodeWriteDictList("I", found, buf))
        return false;
      if(closer.has_value())
      {
        if(!BEncodeWriteDictEntry("K", closer.value(), buf))
          return false;
      }
      if(!BEncodeWriteDictInt("T", txid, buf))
        return false;
      if(!BEncodeWriteDictInt("V", version, buf))
        return false;
      return bencode_end(buf);
    }
  }  // namespace dht
}  // namespace llarp
