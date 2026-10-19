#pragma once

/**
 * S3↔P4 线格式（小端、pack(1)）。与主机文档同一套，ver 固定 0x01。
 *
 * 整帧：
 *   A5 5A | ver=0x01 | msg_type | seq | len(u16 LE) | payload[len] | crc16(LE)
 *
 * crc16：Modbus，文档多项式 0x8005，实现用反射 0xA001，初值 0xFFFF。
 *         覆盖从 ver 到 payload 最后一个字节，不含 magic、不含 CRC 本身。
 * len 只计 payload。新旧 0x20 用 len 区分：必须等于 sizeof(peer_event_t)（136），旧 118 丢弃。
 *
 * msg_type：
 *   0x10 下行瞬时快照 peer_snapshot_t（393）
 *   0x11 下行环境配置 peer_env_cfg_t（26），不要求应答
 *   0x20 上行事件 peer_event_t（136）。P4 主动发，无事件就不发
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PEER_FRAME_MAGIC0     0xA5
#define PEER_FRAME_MAGIC1     0x5A
#define PEER_FRAME_VER        0x01
#define PEER_MSG_SNAPSHOT     0x10
#define PEER_MSG_ENV_CFG      0x11
#define PEER_MSG_EVENT        0x20
/** 一帧上限；0x10 整帧约 402 字节。 */
#define PEER_FRAME_MAX        512
#define PEER_STR16            16
#define PEER_TARGET_MAX       8
#define PEER_EVENT_STR_MAX    32

/** mask 哪一路参与环境事件。 */
#define PEER_ENV_MASK_CO2     (1u << 0)
#define PEER_ENV_MASK_TEMP    (1u << 1)
#define PEER_ENV_MASK_RH      (1u << 2)
#define PEER_ENV_MASK_LUX     (1u << 3)
#define PEER_ENV_MASK_SOUND   (1u << 4)

#pragma pack(push, 1)
/**
 * 0x10 里的一条雷达轨迹，小端。
 * zone_id=0：人在检测范围内，但没进任何业务子区。JSON 里 z 为 null。
 */
typedef struct {
    uint8_t track_id;          /**< 雷达轨迹号，进出区用来认是不是同一个人。 */
    uint8_t kinesia;           /**< 运动状态。本轮六类事件不用，仍原样收下。 */
    uint8_t feature;           /**< 目标特征。本轮判定不用。 */
    int16_t x_cm;              /**< 相对雷达的横坐标，厘米。 */
    int16_t y_cm;              /**< 相对雷达的纵坐标，厘米。 */
    int16_t speed_cm_s;        /**< 速度，厘米/秒。本轮判定不用。 */
    int32_t zone_id;           /**< 业务子区。0 不是区。 */
    char purpose[PEER_STR16];  /**< 该区在网页上的用途，房间级事件留空。 */
    char special[PEER_STR16];  /**< 区域特殊标记，本轮判定不用。 */
} peer_target_t;

/**
 * 0x10 载荷，393 字节。这是瞬时画面，不是事件。
 * 环境读失败为 -1，该路本拍不产生边沿。
 * sound_db_x10 是未校准的近似声级×10，不是 SPL。
 * utc_unix_s 在 S3 对时之前不可信，边沿按帧先后，不按这个时间。
 * 事件墙钟由 S3 收到 0x20 的时刻去打。
 */
typedef struct {
    uint32_t utc_unix_s;       /**< 主机墙上时钟，秒。未对时不要拿来比边沿。 */
    uint8_t radar_ready;       /**< 0：目标不可信，占用冻结；环境仍可边沿。 */
    uint8_t people_room;       /**< 房间里的人数（已去掉干扰源）。进出房只看它。 */
    uint8_t target_n;          /**< targets[] 里有效条数，最多 8。 */
    peer_target_t targets[PEER_TARGET_MAX];
    int16_t co2_ppm;           /**< CO₂，ppm。失败 -1。 */
    int16_t temp_c_x10;        /**< 温度，℃×10。30.0℃ 为 300。失败 -1。 */
    int16_t rh_x10;            /**< 相对湿度，%×10。失败 -1。 */
    int16_t lux;               /**< 照度。失败 -1。 */
    int16_t sound_db_x10;      /**< 近似声级×10。失败 -1。 */
    char room_type[PEER_STR16]; /**< 房间类型，原样抄进事件。 */
} peer_snapshot_t;

/**
 * 0x11 载荷，26 字节。网页 /api/sensors 全机一份，P4 不另做按房间的死表。
 * 温度、湿度、声级在线上已经是 ×10，S3 负责把网页的 ℃、%RH、dB 乘 10。
 * 界值 -1 表示这个界不用。回差必须 >0；收到 0 时判定改用默认回差。
 * 温度只有上限。湿度、照度有上下限。
 * lux 仍是 i16，网页若超过 32767 由主机钳位。
 * CRC 通过后从下一帧 0x10 起生效，本帧不要求应答。
 */
typedef struct {
    uint8_t mask;              /**< bit0 CO₂、1 温度、2 湿度、3 照度、4 声级。关的路不报。 */
    int16_t co2_high_ppm;      /**< 网页 warning_ppm。 */
    int16_t temp_high_x10;     /**< 网页 temp_high，℃×10。没有温度下限。 */
    int16_t rh_high_x10;       /**< 湿度上限，%×10。 */
    int16_t rh_low_x10;        /**< 湿度下限，%×10。 */
    int16_t lux_bright;        /**< 过亮。 */
    int16_t lux_dark;          /**< 过暗。 */
    int16_t sound_high_x10;    /**< 网页 warning_db×10。 */
    int16_t hyst_co2_ppm;      /**< CO₂ 回差。默认 50。 */
    int16_t hyst_temp_x10;     /**< 温度回差，℃×10。默认 5。 */
    int16_t hyst_rh_x10;       /**< 湿度回差，%×10。默认 20。 */
    int16_t hyst_lux;          /**< 照度回差。默认 20。 */
    int16_t hyst_sound_x10;    /**< 声级回差×10。默认 30。 */
    uint8_t reserved;          /**< 填 0。 */
} peer_env_cfg_t;

/** 0x20 里带的目标摘要。只留轨迹号和坐标。 */
typedef struct {
    uint8_t track_id;
    int16_t x_cm;
    int16_t y_cm;
} peer_event_target_t;

/**
 * 0x20 载荷，136 字节。P4 主动发送。
 * env_ch：环境事件写 co2/temp/rh/lux/sound；占用事件全 0。
 * purpose 只放区域用途，不要把通道名塞进来。
 * zone_id!=0 时 targets[] 只留该区；zone_id==0 时留检测范围内全部目标。
 */
typedef struct {
    char event_type[PEER_EVENT_STR_MAX]; /**< 映射表里的英文名。 */
    char env_ch[PEER_STR16];
    int32_t zone_id;                     /**< 房间级为 0；子区事件为该区 id。 */
    char purpose[PEER_STR16];
    uint8_t people_room;
    uint8_t target_n;
    int16_t co2_ppm;
    int16_t temp_c_x10;
    int16_t rh_x10;
    int16_t lux;
    int16_t sound_db_x10;
    char room_type[PEER_STR16];
    peer_event_target_t targets[PEER_TARGET_MAX];
} peer_event_t;
#pragma pack(pop)

/* C 用 _Static_assert。模型任务是 C++，只认 static_assert。 */
#ifdef __cplusplus
static_assert(sizeof(peer_snapshot_t) == 393, "peer_snapshot_t");
static_assert(sizeof(peer_env_cfg_t) == 26, "peer_env_cfg_t");
static_assert(sizeof(peer_event_t) == 136, "peer_event_t");
#else
_Static_assert(sizeof(peer_snapshot_t) == 393, "peer_snapshot_t");
_Static_assert(sizeof(peer_env_cfg_t) == 26, "peer_env_cfg_t");
_Static_assert(sizeof(peer_event_t) == 136, "peer_event_t");
#endif

/** 覆盖 data[0..len)。结果小端放在帧尾。 */
uint16_t peer_crc16(const uint8_t *data, size_t len);

/**
 * 组一帧。成功返回总字节数（7 + payload + 2），缓冲不够或 payload 为空指针时返回 0。
 * seq 原样写入，P4 发 0x20 时用刚收到的 0x10 的 seq。
 */
size_t peer_frame_encode(uint8_t msg_type, uint8_t seq, const uint8_t *payload, uint16_t payload_len,
                         uint8_t *out, size_t out_max);

/**
 * buf 必须从 A5 5A 开始。ver 不是 0x01、CRC 不对、长度超出缓冲都返回 false。
 * 成功时 payload 指向 buf 内部，调用方不要释放。
 */
bool peer_frame_decode(const uint8_t *buf, size_t buf_len, uint8_t *msg_type, uint8_t *seq,
                       const uint8_t **payload, uint16_t *payload_len);

/** 整段缓冲里找第一帧能通过 CRC 的 A5 5A。找不到返回 false，不告诉调用方垃圾有多长。 */
bool peer_frame_scan_decode(const uint8_t *buf, size_t buf_len, uint8_t *msg_type, uint8_t *seq,
                            const uint8_t **payload, uint16_t *payload_len);

/**
 * UART 字节流拆帧。一次只取下一帧，调用方按 *drop 丢掉前缀后再调。
 * 成功：payload 指向 buf 里这一帧，*drop 是帧起始前的垃圾字节数。
 * 失败且 *drop==0：后面字节还不够一帧，先留着。
 * 失败且 *drop>0：CRC 错或假帧头，丢掉这么多字节再扫。CRC 错只丢 magic 的第一字节，避免把真正的 A5 吃掉。
 */
bool peer_frame_next(const uint8_t *buf, size_t buf_len, size_t *drop, uint8_t *msg_type,
                     uint8_t *seq, const uint8_t **payload, uint16_t *payload_len);
