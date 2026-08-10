/**
 * @file hal_tp.hpp
 * @author Forairaaaaa
 * @brief 
 * @version 0.1
 * @date 2023-05-20
 * 
 * @copyright Copyright (c) 2023
 * 
 */
#pragma once
#include "hal_pins.h"
#include <driver/gpio.h>
#include <LovyanGFX.hpp>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <cstring>

/* ⚠️ **旧 `driver/i2c.h` は使えない。**
   LovyanGFX 1.2.26 が ESP-IDF 5.5 の新I2Cドライバ（`i2c_new_master_bus`）を掴んでおり、
   旧ドライバのシンボルが同じバイナリに**リンクされるだけで**起動時にabortする
   （実機をブートループさせて確認）。よってI2Cの足回りは
   **LovyanGFX の `lgfx::i2c` に寄せてある**——同じドライバに揃うので衝突しない。
   ⚠️ ESPHomeの `i2c:` コンポーネントも旧ドライバ側なので、そちらへは寄せられない。 */
namespace TP_I2C
{
    static constexpr uint32_t FREQ = 100000;  /* 100kHz */
}


/** @brief FT5x06 register map and function codes */
#define FT5x06_ADDR                    (0x38)

#define FT5x06_DEVICE_MODE             (0x00)
#define FT5x06_GESTURE_ID              (0x01)
#define FT5x06_TOUCH_POINTS            (0x02)

#define FT5x06_TOUCH1_EV_FLAG          (0x03)
#define FT5x06_TOUCH1_XH               (0x03)
#define FT5x06_TOUCH1_XL               (0x04)
#define FT5x06_TOUCH1_YH               (0x05)
#define FT5x06_TOUCH1_YL               (0x06)

#define FT5x06_TOUCH2_EV_FLAG          (0x09)
#define FT5x06_TOUCH2_XH               (0x09)
#define FT5x06_TOUCH2_XL               (0x0A)
#define FT5x06_TOUCH2_YH               (0x0B)
#define FT5x06_TOUCH2_YL               (0x0C)

#define FT5x06_TOUCH3_EV_FLAG          (0x0F)
#define FT5x06_TOUCH3_XH               (0x0F)
#define FT5x06_TOUCH3_XL               (0x10)
#define FT5x06_TOUCH3_YH               (0x11)
#define FT5x06_TOUCH3_YL               (0x12)

#define FT5x06_TOUCH4_EV_FLAG          (0x15)
#define FT5x06_TOUCH4_XH               (0x15)
#define FT5x06_TOUCH4_XL               (0x16)
#define FT5x06_TOUCH4_YH               (0x17)
#define FT5x06_TOUCH4_YL               (0x18)

#define FT5x06_TOUCH5_EV_FLAG          (0x1B)
#define FT5x06_TOUCH5_XH               (0x1B)
#define FT5x06_TOUCH5_XL               (0x1C)
#define FT5x06_TOUCH5_YH               (0x1D)
#define FT5x06_TOUCH5_YL               (0x1E)

#define FT5x06_ID_G_THGROUP            (0x80)
#define FT5x06_ID_G_THPEAK             (0x81)
#define FT5x06_ID_G_THCAL              (0x82)
#define FT5x06_ID_G_THWATER            (0x83)
#define FT5x06_ID_G_THTEMP             (0x84)
#define FT5x06_ID_G_THDIFF             (0x85)
#define FT5x06_ID_G_CTRL               (0x86)
#define FT5x06_ID_G_TIME_ENTER_MONITOR (0x87)
#define FT5x06_ID_G_PERIODACTIVE       (0x88)
#define FT5x06_ID_G_PERIODMONITOR      (0x89)
#define FT5x06_ID_G_AUTO_CLB_MODE      (0xA0)
#define FT5x06_ID_G_LIB_VERSION_H      (0xA1)
#define FT5x06_ID_G_LIB_VERSION_L      (0xA2)
#define FT5x06_ID_G_CIPHER             (0xA3)
#define FT5x06_ID_G_MODE               (0xA4)
#define FT5x06_ID_G_PMODE              (0xA5)
#define FT5x06_ID_G_FIRMID             (0xA6)
#define FT5x06_ID_G_STATE              (0xA7)
#define FT5x06_ID_G_FT5201ID           (0xA8)
#define FT5x06_ID_G_ERR                (0xA9)


namespace FT3267
{
    static const char* TAG = "ft3267";


    enum ft3267_gesture_t
    {
        ft3267_gesture_none         = 0x00,
        ft3267_gesture_move_up      = 0x10,
        ft3267_gesture_move_left    = 0x14,
        ft3267_gesture_move_down    = 0x18,
        ft3267_gesture_move_right   = 0x1c,
        ft3267_gesture_zoom_in      = 0x48,
        ft3267_gesture_zoom_out     = 0x49,
    };


    struct TouchPoint_t
    {
        uint8_t touch_num = 0;
        int x = -1;
        int y = -1;
    };


    struct Config_t
    {
        int i2c_port = 0;   /* lgfx::i2c はポート番号をintで取る */
        uint8_t dev_addr = FT5x06_ADDR;
    };


    class TP_FT3267
    {
        private:
            Config_t _cfg;
            uint8_t _data_buffer[7];
            TouchPoint_t _touch_point_buffer;
            uint8_t _i2c_fail_count = 0;
            bool _recovering = false;

            /* G_CTRL（0x86）の見張り。放置でタッチが死ぬ故障の観測点。
               死んだ機体をUSBに挿すと電源が切れて直るのでシリアルでの事後調査は不可能——
               値はHAへ送るしかない。httpdタスクから読まれるので volatile でキャッシュする
               （I2Cはメインタスクの専有。getterはキャッシュを返すだけでI2Cに触らない）。 */
            static constexpr uint32_t G_CTRL_POLL_MS = 60000;
            volatile int16_t  _g_ctrl_boot = -1;   /* 起動直後の読み返し。-1 = 読めなかった */
            volatile int16_t  _g_ctrl_last = -1;   /* 直近の読み返し */
            volatile uint16_t _g_ctrl_bad = 0;     /* 0以外を観測した累計回数 */
            /* 鮮度。**「ずっと0」を読み違えないための計器**——pongを返すのはhttpdタスクで、
               ウォッチドッグも `_last_ping_ms` しか見ていないため、**メインタスクが止まっても
               心拍は健康なまま古いキャッシュを送り続ける**。この数が伸びていなければ
               「0だった」ではなく「読んでいない」。 */
            volatile uint32_t _g_ctrl_polls = 0;
            /* 直近のタッチポイント数レジスタ（生値）。**非0に張り付く故障がある**——
               そのとき「ずっと指が触れている」ように見え、G_CTRLは0のままタッチだけ死ぬ。
               その故障機序をG_CTRL仮説と切り分けるために持つ。 */
            volatile uint8_t _tp_points_last = 0;
            /* ⚠️ **直近の読み取りが成功したか。** `getTouchPointsNum()` は読めなかったときも
               `0` を返すので、**戻り値だけでは「指が無い」と区別できない**（失敗はログには
               出るが、呼び元には伝わらない）。押下と離上を追う側は、必ずこちらを先に見る——
               離上を意味のある事象として扱う瞬間から、この曖昧さは
               「通信が1回こけただけで指を離したことにする」バグになる。 */
            volatile bool _tp_read_ok = true;
            /* 読めなかった累計。**0より大きくなったら、上の区別が実際に要ったということ。** */
            volatile uint16_t _tp_read_fail = 0;
            bool     _g_ctrl_boot_seen = false;
            uint32_t _g_ctrl_next_poll_ms = 0;


            inline void _wake_pulse()
            {
                /* INTピンをLowに引いてFT3267をMonitor/Sleepから起こす（LovyanGFX Touch_FT5x06::wakeup と同方式） */
                gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT);
                gpio_set_level(GPIO_NUM_14, 0);
                esp_rom_delay_us(600);
                gpio_set_direction(GPIO_NUM_14, GPIO_MODE_INPUT);
                gpio_set_pull_mode(GPIO_NUM_14, GPIO_PULLUP_ONLY);
            }


            inline bool _recover_i2c()
            {
                if (_recovering) return false;

                _recovering = true;
                ESP_LOGW(TAG, "reinitializing touch I2C");
                _wake_pulse();
                lgfx::i2c::release(_cfg.i2c_port);
                if (!lgfx::i2c::init(_cfg.i2c_port, HAL_PIN_TP_I2C_SDA, HAL_PIN_TP_I2C_SCL).has_value())
                {
                    ESP_LOGE(TAG, "touch I2C reinit failed");
                    _recovering = false;
                    return false;
                }

                _tp_init();
                _recovering = false;
                return true;
            }


            /* ⚠️ 引数が esp_err_t から bool へ変わった（lgfx::i2c は cpp::result を返すため）。
               **3回続けて失敗したら作り直す**という復旧の意味は変えていない。 */
            inline bool _handle_i2c_result(const char* op, bool ok)
            {
                if (ok)
                {
                    _i2c_fail_count = 0;
                    return true;
                }

                ESP_LOGW(TAG, "%s failed", op);
                if (++_i2c_fail_count < 3)
                {
                    return false;
                }

                _i2c_fail_count = 0;
                _recover_i2c();
                return false;
            }


            inline bool _writr_reg(uint8_t reg, uint8_t data)
            {
                return _handle_i2c_result("touch write",
                    lgfx::i2c::writeRegister8(_cfg.i2c_port, _cfg.dev_addr, reg, data, 0, TP_I2C::FREQ).has_value());
            }


            inline bool _read_reg(uint8_t reg, uint8_t readSize)
            {
                /* Store data into buffer */
                return _handle_i2c_result("touch read",
                    lgfx::i2c::readRegister(_cfg.i2c_port, _cfg.dev_addr, reg, _data_buffer, readSize, TP_I2C::FREQ).has_value());
            }


            /**
             * @brief G_CTRLを読み返し、0以外なら0x00を書き直す。戻り値は読めた値（-1=読めず）。
             *
             * Monitorモード自動遷移が有効（=0以外）に戻っていたら、それが放置死の直前の姿。
             * 書き直しは仮説の真偽に関わらず受け皿として効く。**呼べるのはメインタスクだけ**。
             */
            inline int16_t _check_g_ctrl()
            {
                /* 読めても読めなくても数える。**試行の回数**であって成功の回数ではない
                   （止まっているのか読めていないのかを分けるのが役目）。 */
                _g_ctrl_polls = _g_ctrl_polls + 1;
                if (!_read_reg(FT5x06_ID_G_CTRL, 1))
                {
                    ESP_LOGW(TAG, "touch G_CTRL readback failed");
                    _g_ctrl_last = -1;
                    return -1;
                }

                const uint8_t ctrl = _data_buffer[0];
                _g_ctrl_last = static_cast<int16_t>(ctrl);
                if (ctrl != 0x00)
                {
                    if (_g_ctrl_bad < 0xFFFF) _g_ctrl_bad = _g_ctrl_bad + 1;
                    ESP_LOGW(TAG, "touch G_CTRL is 0x%02X; rewriting 0x00", ctrl);
                    const bool rewritten = _writr_reg(FT5x06_ID_G_CTRL, 0x00);
                    ESP_LOGW(TAG, "touch G_CTRL rewrite %s", rewritten ? "succeeded" : "failed");
                }
                return static_cast<int16_t>(ctrl);
            }


            /* 毎フレーム呼ばれる経路に相乗りさせる間引き読み。専用タスクを立てないのは
               _data_buffer が無保護で、TPのI2Cをメインタスクの専有にしておく必要があるため。 */
            inline void _poll_g_ctrl()
            {
                const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
                if (static_cast<int32_t>(now - _g_ctrl_next_poll_ms) < 0) return;
                _g_ctrl_next_poll_ms = now + G_CTRL_POLL_MS;
                _check_g_ctrl();
            }


            inline void _tp_init()
            {
                // Valid touching detect threshold
                _writr_reg(FT5x06_ID_G_THGROUP, 70);

                // valid touching peak detect threshold
                _writr_reg(FT5x06_ID_G_THPEAK, 60);

                // Touch focus threshold
                _writr_reg(FT5x06_ID_G_THCAL, 16);

                // threshold when there is surface water
                _writr_reg(FT5x06_ID_G_THWATER, 60);

                // threshold of temperature compensation
                _writr_reg(FT5x06_ID_G_THTEMP, 10);

                // Touch difference threshold
                _writr_reg(FT5x06_ID_G_THDIFF, 20);

                // Monitorモード自動遷移を無効化（常時Active固定）。
                // デフォルト(0x86=1)だと放置時にMonitorへ自動遷移し、個体によって復帰に失敗して
                // タッチが無反応になる（長時間放置死バグの根本原因）。常時給電なので常時Activeで問題ない。
                _writr_reg(FT5x06_ID_G_CTRL, 0x00);

                const int16_t ctrl = _check_g_ctrl();
                ESP_LOGW(TAG, "touch G_CTRL readback: %d", static_cast<int>(ctrl));
                if (!_g_ctrl_boot_seen)
                {
                    /* _tp_init() は _recover_i2c() からも呼ばれる。boot値は最初の一度だけ確定させ、
                       以後の再initで上書きしない（「起動時に一度でも0以外か」が仮説の判定材料）。 */
                    _g_ctrl_boot_seen = true;
                    _g_ctrl_boot = ctrl;
                }
                _g_ctrl_next_poll_ms =
                    static_cast<uint32_t>(esp_timer_get_time() / 1000) + G_CTRL_POLL_MS;
            }


        public:
            TP_FT3267()
            {
                memset(_data_buffer, 0, sizeof(_data_buffer));
            }
            ~TP_FT3267() = default;

            
            /* Config */
            inline Config_t getConfig() { return _cfg; }
            inline void setConfig(const Config_t& cfg) { _cfg = cfg; }


            inline bool init()
            {
                ESP_LOGI(TAG, "init tp ft3267");
                
                /* Interrupt pin */
                gpio_reset_pin(GPIO_NUM_14);
                gpio_set_direction(GPIO_NUM_14, GPIO_MODE_INPUT);
                gpio_set_pull_mode(GPIO_NUM_14, GPIO_PULLUP_ONLY);

                _i2c_fail_count = 0;
                _tp_init();

                return true;
            }


            inline uint8_t getTouchPointsNum()
            {
                /* isTouched() 経由でどのループからも毎フレーム通る唯一の場所。
                   放置中（_idle_loop）も通るので、見張りたい時間帯がちょうど覆える。 */
                _poll_g_ctrl();


                _data_buffer[0] = 0;
                if (!_read_reg(FT5x06_TOUCH_POINTS, 1))
                {
                    /* ⚠️ **0を返すが、これは「指が無い」ではない。**
                       呼び元は `getTouchReadOk()` で分けること。 */
                    _tp_read_ok = false;
                    if (_tp_read_fail < 0xFFFF) _tp_read_fail = _tp_read_fail + 1;
                    return 0;
                }
                _tp_read_ok = true;
                _tp_points_last = _data_buffer[0];
                return _data_buffer[0];
            }


            inline const TouchPoint_t& readPos()
            {
                _touch_point_buffer.touch_num = 0;
                _touch_point_buffer.x = -1;
                _touch_point_buffer.y = -1;

                /* Get touch num */
                if (!_read_reg(FT5x06_TOUCH_POINTS, 1))
                {
                    return _touch_point_buffer;
                }
                _data_buffer[0] = _data_buffer[0] & 0x0F;
                _touch_point_buffer.touch_num = _data_buffer[0];

                /* Get postion */
                if (_data_buffer[0] != 0)
                {
                    if (!_read_reg(FT5x06_TOUCH1_XH, 4))
                    {
                        return _touch_point_buffer;
                    }
                    _touch_point_buffer.x = ((_data_buffer[0] & 0x0f) << 8) + _data_buffer[1];
                    _touch_point_buffer.y = ((_data_buffer[2] & 0x0f) << 8) + _data_buffer[3];
                }

                return _touch_point_buffer;
            }


            inline bool isTouched()
            {
                return (getTouchPointsNum() > 0);
            }


            /* G_CTRLの見張り結果。**キャッシュを返すだけでI2Cに触らない**ので、
               httpdタスク（pongへの相乗り）から呼んでよい唯一の口。 */
            inline int16_t  getGCtrlBoot() const { return _g_ctrl_boot; }
            inline int16_t  getGCtrlLast() const { return _g_ctrl_last; }
            inline uint16_t getGCtrlBadCount() const { return _g_ctrl_bad; }
            inline uint32_t getGCtrlPollCount() const { return _g_ctrl_polls; }
            inline uint8_t  getTouchPointsRaw() const { return _tp_points_last; }
            /** ⚠️ **直前の `getTouchPointsNum()` が読めたか。** `false` のとき戻り値の `0` に
             *  意味は無い。押下・離上を追う側は、そのフレームを丸ごと捨てること。 */
            inline bool     getTouchReadOk() const { return _tp_read_ok; }
            /** 読めなかった累計。 */
            inline uint16_t getTouchReadFailCount() const { return _tp_read_fail; }


            /**
             * @brief Update internal touch point buffer
             * 
             */
            inline void update()
            {
                readPos();
            }


            /**
             * @brief Get internal touch point buffer
             * 
             * @return TouchPoint_t 
             */
            inline TouchPoint_t getTouchPointBuffer()
            {
                return _touch_point_buffer;
            }
    };


}
