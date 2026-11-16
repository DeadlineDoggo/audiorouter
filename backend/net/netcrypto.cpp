#include "netcrypto.h"

#include <sodium.h>

namespace NetCrypto {

bool init()
{
    return sodium_init() >= 0;   // 1 = already initialised
}

ChannelKeys deriveKeys(const QString &channel, const QString &passphrase)
{
    ChannelKeys keys;
    if (!init()) return keys;

    const QByteArray chan = channel.trimmed().normalized(QString::NormalizationForm_C).toUtf8();
    const QByteArray pass = passphrase.trimmed().normalized(QString::NormalizationForm_C).toUtf8();
    if (chan.isEmpty() || pass.isEmpty()) return keys;

    // The channel name is the salt: the same passphrase on two channels
    // gives unrelated keys.
    unsigned char salt[crypto_pwhash_SALTBYTES];
    const QByteArray saltInput = QByteArrayLiteral("soundroot-net-v1") + '\0' + chan;
    crypto_generichash(salt, sizeof salt,
                       reinterpret_cast<const unsigned char *>(saltInput.constData()),
                       static_cast<unsigned long long>(saltInput.size()), nullptr, 0);

    // Fixed parameters: they are part of the protocol, every implementation
    // must use the same ones.
    unsigned char master[crypto_kdf_KEYBYTES];
    if (crypto_pwhash(master, sizeof master, pass.constData(),
                      static_cast<unsigned long long>(pass.size()), salt,
                      2 /* ops */, 64ULL * 1024 * 1024 /* bytes */,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        return keys;   // out of memory
    }

    unsigned char aead[kKeyBytes];
    unsigned char chanId[16];
    crypto_kdf_derive_from_key(aead, sizeof aead, 1, "srnaead1", master);
    crypto_kdf_derive_from_key(chanId, sizeof chanId, 2, "srnchan1", master);

    keys.aeadKey = QByteArray(reinterpret_cast<const char *>(aead), kKeyBytes);
    keys.channelId = QByteArray(reinterpret_cast<const char *>(chanId), kChannelIdBytes);

    sodium_memzero(master, sizeof master);
    sodium_memzero(aead, sizeof aead);
    return keys;
}

QByteArray seal(const QByteArray &key, const QByteArray &nonce,
                const QByteArray &associatedData, const QByteArray &plaintext)
{
    if (key.size() != kKeyBytes || nonce.size() != kNonceBytes) return {};

    QByteArray out(plaintext.size() + kTagBytes, Qt::Uninitialized);
    unsigned long long outLen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(out.data()), &outLen,
        reinterpret_cast<const unsigned char *>(plaintext.constData()),
        static_cast<unsigned long long>(plaintext.size()),
        reinterpret_cast<const unsigned char *>(associatedData.constData()),
        static_cast<unsigned long long>(associatedData.size()),
        nullptr,
        reinterpret_cast<const unsigned char *>(nonce.constData()),
        reinterpret_cast<const unsigned char *>(key.constData()));
    out.resize(static_cast<qsizetype>(outLen));
    return out;
}

bool open(const QByteArray &key, const QByteArray &nonce,
          const QByteArray &associatedData, const QByteArray &ciphertext,
          QByteArray *plaintext)
{
    if (key.size() != kKeyBytes || nonce.size() != kNonceBytes
        || ciphertext.size() < kTagBytes || !plaintext)
        return false;

    QByteArray out(ciphertext.size() - kTagBytes, Qt::Uninitialized);
    unsigned long long outLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(out.data()), &outLen, nullptr,
            reinterpret_cast<const unsigned char *>(ciphertext.constData()),
            static_cast<unsigned long long>(ciphertext.size()),
            reinterpret_cast<const unsigned char *>(associatedData.constData()),
            static_cast<unsigned long long>(associatedData.size()),
            reinterpret_cast<const unsigned char *>(nonce.constData()),
            reinterpret_cast<const unsigned char *>(key.constData())) != 0) {
        return false;
    }
    out.resize(static_cast<qsizetype>(outLen));
    *plaintext = out;
    return true;
}

QByteArray randomBytes(int count)
{
    init();
    QByteArray out(count, Qt::Uninitialized);
    randombytes_buf(out.data(), static_cast<size_t>(count));
    return out;
}

quint64 randomU64()
{
    init();
    quint64 v = 0;
    randombytes_buf(&v, sizeof v);
    return v;
}

QString generatePassphrase()
{
    init();
    // No 0/O, 1/I/L: easy to read out and type
    static const char alphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789a";   // 32 symbols
    QString out;
    for (int i = 0; i < 20; ++i) {
        if (i > 0 && i % 5 == 0) out += QLatin1Char('-');
        out += QLatin1Char(alphabet[randombytes_uniform(32)]);
    }
    return out;
}

} // namespace NetCrypto
