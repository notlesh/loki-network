#ifndef LLARP_CRYPTO_TYPES_HPP
#define LLARP_CRYPTO_TYPES_HPP

#include <crypto/constants.hpp>
#include <router_id.hpp>
#include <util/aligned.hpp>
#include <util/types.hpp>

#include <algorithm>
#include <iostream>

namespace llarp
{
  using SharedSecret     = AlignedBuffer< SHAREDKEYSIZE >;
  using KeyExchangeNonce = AlignedBuffer< 32 >;

  struct PubKey final : public AlignedBuffer< PUBKEYSIZE >
  {
    PubKey() = default;

    explicit PubKey(const byte_t *ptr) : AlignedBuffer< SIZE >(ptr)
    {
    }

    explicit PubKey(const Data &data) : AlignedBuffer< SIZE >(data)
    {
    }

    explicit PubKey(const AlignedBuffer< SIZE > &other)
        : AlignedBuffer< SIZE >(other)
    {
    }

    std::string
    ToString() const;

    bool
    FromString(const std::string &str);

    operator RouterID() const
    {
      return RouterID(as_array());
    }

    PubKey &
    operator=(const byte_t *ptr)
    {
      std::copy(ptr, ptr + SIZE, begin());
      return *this;
    }
  };

  inline std::ostream &
  operator<<(std::ostream &out, const PubKey &k)
  {
    return out << k.ToString();
  }

  inline bool
  operator==(const PubKey &lhs, const PubKey &rhs)
  {
    return lhs.as_array() == rhs.as_array();
  }

  inline bool
  operator==(const PubKey &lhs, const RouterID &rhs)
  {
    return lhs.as_array() == rhs.as_array();
  }

  inline bool
  operator==(const RouterID &lhs, const PubKey &rhs)
  {
    return lhs.as_array() == rhs.as_array();
  }

  struct PrivateKey;

  /// Stores a sodium "secret key" value, which is actually the seed concatenated with the public
  /// key.  Note that the seed is *not* the private key value itself, but rather the seed from which
  /// it can be calculated.
  struct SecretKey final : public AlignedBuffer< SECKEYSIZE >
  {
    SecretKey() = default;

    explicit SecretKey(const byte_t *ptr) : AlignedBuffer< SECKEYSIZE >(ptr)
    {
    }

    // The full data
    explicit SecretKey(const AlignedBuffer< SECKEYSIZE > &seed)
      : AlignedBuffer< SECKEYSIZE >(seed)
    {}

    // Just the seed, we recalculate the pubkey
    explicit SecretKey(const AlignedBuffer< 32 > &seed)
    {
      std::copy(seed.begin(), seed.end(), begin());
      Recalculate();
    }

    /// recalculate public component
    bool
    Recalculate();

    std::ostream &
    print(std::ostream &stream, int level, int spaces) const
    {
      Printer printer(stream, level, spaces);
      printer.printValue("secretkey");
      return stream;
    }

    PubKey
    toPublic() const
    {
      return PubKey(data() + 32);
    }

    /// Computes the private key from the secret key (which is actually the seed)
    bool
    toPrivate(PrivateKey& key) const;

    bool
    LoadFromFile(const char *fname);

    bool
    SaveToFile(const char *fname) const;
  };

  inline std::ostream &
  operator<<(std::ostream &out, const SecretKey &)
  {
    // return out << k.ToHex();
    // make sure we never print out secret keys
    return out << "[secretkey]";
  }

  /// PrivateKey is similar to SecretKey except that it only stores the private key value
  /// itself unlike SecretKey which stores the seed from which the private key value is generated.
  /// This is intended for use with derived keys, where we can derive the private key but not the
  /// seed.
  struct PrivateKey final : public AlignedBuffer< 32 >
  {
    PrivateKey() = default;

    explicit PrivateKey(const byte_t *ptr) : AlignedBuffer< 32 >(ptr)
    {
    }

    explicit PrivateKey(const AlignedBuffer< 32 > &seed) : AlignedBuffer< 32>(seed) {}

    std::ostream &
    print(std::ostream &stream, int level, int spaces) const
    {
      Printer printer(stream, level, spaces);
      printer.printValue("privatekey");
      return stream;
    }

    /// Computes the public key
    bool
    toPublic(PubKey& pubkey) const;
  };

  inline std::ostream &
  operator<<(std::ostream &out, const PrivateKey &)
  {
    // return out << k.ToHex();
    // make sure we never print out private keys
    return out << "[privatekey]";
  }

  /// A subkey differs from a normal, root-level key in that it stores the actual ed25519 private
  /// scalar as well as different randomness for key signing.
  ///
  /// A normal ed25519 "private key" (as libsodium treats it) is really a seed that generates both
  /// the private scalar and randomness from the hash of the seed.
  ///
  /// Since our subkeys aren't generated from seed, we need another way to generate them, and also
  /// a way to explicitly store them. This class accomplishes this by storing the 32 byte scalar
  /// alongside the random value.
  struct SubSecretKey final : public AlignedBuffer< 64 >
  {
    SubSecretKey() = default;

    explicit SubSecretKey(const AlignedBuffer< 32 > &scalar)
    {
      SetScalar(scalar);
    }

    /// Sets the scalar and recalculates random value
    bool
    SetScalar(const AlignedBuffer< 32 > &scalar);

    /// Regenerates the random value used in signing
    bool
    Recalculate();

    /// Returns a pointer to the scalar (secret)
    byte_t*
    GetScalar() { return data(); }

    /// Returns a pointer to the random value
    byte_t*
    GetRandomValue() { return data() + 32; }

    /// Computes the public key
    bool
    toPublic(PubKey& pubkey) const;

    // prevent printing our secret
    std::ostream &
    print(std::ostream &stream, int level, int spaces) const
    {
      Printer printer(stream, level, spaces);
      printer.printValue("privatesubkey");
      return stream;
    }
  };

  inline std::ostream &
  operator<<(std::ostream &out, const SubSecretKey &)
  {
    // return out << k.ToHex();
    // make sure we never print out private keys
    return out << "[subsecretkey]";
  }

  /// IdentitySecret is a secret key from a service node secret seed
  struct IdentitySecret final : public AlignedBuffer< 32 >
  {
    IdentitySecret() : AlignedBuffer< 32 >()
    {
    }

    /// no copy constructor
    explicit IdentitySecret(const IdentitySecret &) = delete;
    // no byte data constructor
    explicit IdentitySecret(const byte_t *) = delete;

    /// load service node seed from file
    bool
    LoadFromFile(const char *fname);
  };

  inline std::ostream &
  operator<<(std::ostream &out, const IdentitySecret &)
  {
    // make sure we never print out secret keys
    return out << "[IdentitySecret]";
  }

  using ShortHash = AlignedBuffer< SHORTHASHSIZE >;
  using LongHash  = AlignedBuffer< HASHSIZE >;

  struct Signature final : public AlignedBuffer< SIGSIZE >
  {
    byte_t *
    Hi();

    const byte_t *
    Hi() const;

    byte_t *
    Lo();

    const byte_t *
    Lo() const;
  };

  using TunnelNonce = AlignedBuffer< TUNNONCESIZE >;
  using SymmNonce   = AlignedBuffer< NONCESIZE >;
  using SymmKey     = AlignedBuffer< 32 >;

  using PQCipherBlock = AlignedBuffer< PQ_CIPHERTEXTSIZE + 1 >;
  using PQPubKey      = AlignedBuffer< PQ_PUBKEYSIZE >;
  using PQKeyPair     = AlignedBuffer< PQ_KEYPAIRSIZE >;

  /// PKE(result, publickey, secretkey, nonce)
  using path_dh_func = std::function< bool(
      SharedSecret &, const PubKey &, const SecretKey &, const TunnelNonce &) >;

  /// TKE(result, publickey, secretkey, nonce)
  using transport_dh_func = std::function< bool(
      SharedSecret &, const PubKey &, const SecretKey &, const TunnelNonce &) >;

  /// SH(result, body)
  using shorthash_func =
      std::function< bool(ShortHash &, const llarp_buffer_t &) >;
}  // namespace llarp

#endif
