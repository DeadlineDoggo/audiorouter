#pragma once

#include <QByteArray>
#include <QHostAddress>

#include "net/netcrypto.h"

/**
 * @brief Wire format of the SoundRoot network protocol, version 1.
 *
 * Every datagram is a 32-byte cleartext header followed by the sealed
 * payload.  The header is authenticated together with the payload, so
 * nothing in a packet can be changed without the channel key.
 *
 *   0  "SRN1"            magic + protocol version
 *   4  type (1 byte)     PacketType
 *   5  flags (1 byte)    unused, 0
 *   6  reserved (2)      0
 *   8  channel id (8)    derived from the key; tells channels apart
 *  16  sender id (8)     random per node start
 *  24  counter (8, BE)   strictly increasing per sender; never reused
 *  32  ciphertext + 16-byte tag
 *
 * The nonce is sender id + counter + 8 zero bytes.
 * docs/NETWORK_NODES.md describes the protocol as a whole.
 */
namespace NetProtocol {

constexpr quint16 kDiscoveryPort   = 47780;   ///< beacons are multicast to this port
constexpr quint16 kDefaultDataPort = 47781;   ///< preferred port for everything else
constexpr int     kHeaderBytes     = 32;

constexpr int kSampleRate      = 48000;
constexpr int kChannels        = 2;
/// 5 ms.  960 bytes of audio + 64 bytes of protocol stay well below the
/// 1472-byte UDP payload of an Ethernet/Wi-Fi frame, so packets are never
/// fragmented (a lost fragment would cost the whole packet).
constexpr int kFramesPerPacket = 240;
constexpr int kBytesPerFrame   = kChannels * 2;

/// Administratively scoped (site-local) multicast group for beacons.
inline QHostAddress multicastGroup() { return QHostAddress(QStringLiteral("239.255.77.80")); }

enum PacketType : quint8 {
    Beacon   = 1,   ///< "I am on this channel" - periodic, also keeps peers alive
    Audio    = 2,
    Bye      = 3,   ///< leaving the channel
    Hello    = 4,   ///< challenge: carries 8 random bytes
    HelloAck = 5,   ///< answer: echoes them, proving the sender is live at that address
};

struct Header {
    PacketType type = Beacon;
    quint8     flags = 0;
    QByteArray channelId;   ///< NetCrypto::kChannelIdBytes
    quint64    senderId = 0;
    quint64    counter = 0;
};

/// Reads the header of a datagram.  False when it is not a SoundRoot packet.
bool decodeHeader(const QByteArray &datagram, Header *header);

/// Header + sealed payload, ready to send.
QByteArray sealPacket(const NetCrypto::ChannelKeys &keys, const Header &header,
                      const QByteArray &payload);
/// Verify and decrypt a datagram whose header was read with decodeHeader().
bool openPacket(const NetCrypto::ChannelKeys &keys, const QByteArray &datagram,
                const Header &header, QByteArray *payload);

// ── Audio payload ───────────────────────────────────────────────────────
//   0 codec (1)      0 = PCM signed 16-bit little-endian
//   1 channels (1)
//   2 reserved (2)
//   4 sample rate (4, BE)
//   8 timestamp (8, BE)   sender's frame counter of the first frame
//  16 samples
constexpr quint8 kCodecPcm16 = 0;

struct AudioPayload {
    quint8     codec = kCodecPcm16;
    quint8     channels = kChannels;
    quint32    sampleRate = kSampleRate;
    qint64     timestamp = 0;
    QByteArray samples;
};

QByteArray encodeAudio(qint64 timestamp, const QByteArray &pcm);
bool       decodeAudio(const QByteArray &payload, AudioPayload *audio);

/**
 * @brief Rejects packets that were already seen (anti-replay), tolerating
 *        the reordering a network causes.  Same scheme as IPsec/DTLS.
 */
class ReplayWindow {
public:
    /// True the first time a counter is seen; false for repeats and for
    /// counters more than 64 behind the newest.
    bool accept(quint64 counter);

private:
    bool    m_any = false;
    quint64 m_highest = 0;
    quint64 m_seen = 0;   // bit i set: counter (m_highest - i) was seen
};

/// Loopback, link-local and private (RFC 1918, CGNAT) addresses only.
bool isLocalNetworkAddress(const QHostAddress &address);

} // namespace NetProtocol
