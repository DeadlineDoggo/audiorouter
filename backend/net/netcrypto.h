#pragma once

#include <QByteArray>
#include <QString>

/**
 * @brief Cryptography of the SoundRoot network protocol (libsodium).
 *
 * A channel is a name plus a passphrase.  Both sides derive the same key
 * from them (Argon2id), so only devices that know the passphrase can find
 * each other, read the audio or inject any.  Packets are sealed with
 * XChaCha20-Poly1305.  See docs/NETWORK_NODES.md for the wire format.
 */
namespace NetCrypto {

constexpr int kKeyBytes       = 32;
constexpr int kChannelIdBytes = 8;
constexpr int kNonceBytes     = 24;
constexpr int kTagBytes       = 16;

struct ChannelKeys {
    QByteArray aeadKey;     ///< kKeyBytes; seals every packet
    QByteArray channelId;   ///< kChannelIdBytes; public tag to tell channels apart
    bool isValid() const { return aeadKey.size() == kKeyBytes && channelId.size() == kChannelIdBytes; }
};

/// Must succeed once before anything else is used.  Thread-safe.
bool init();

/// Derive a channel's keys.  Deliberately slow (about 0.1 s, 64 MiB).
ChannelKeys deriveKeys(const QString &channel, const QString &passphrase);

/// Encrypt and authenticate.  Returns ciphertext + tag, empty on error.
QByteArray seal(const QByteArray &key, const QByteArray &nonce,
                const QByteArray &associatedData, const QByteArray &plaintext);
/// Verify and decrypt.  Returns false when the packet is not authentic.
bool open(const QByteArray &key, const QByteArray &nonce,
          const QByteArray &associatedData, const QByteArray &ciphertext,
          QByteArray *plaintext);

QByteArray randomBytes(int count);
quint64    randomU64();
/// A passphrase a person can read out: 4 groups of 5 characters, 100 bits.
QString    generatePassphrase();

} // namespace NetCrypto
