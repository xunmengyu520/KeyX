#include "turbo.hpp"
#include <minIni.h>
#include <cstdlib>
#include <cstring>

namespace {
    // 左 JoyCon 按键掩码（十字键、左肩键、左摇杆、SELECT）
    constexpr u64 LEFT_JOYCON_BUTTONS =
        HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_Up | HidNpadButton_Down |
        HidNpadButton_L | HidNpadButton_ZL | HidNpadButton_StickL |
        HidNpadButton_Minus;

    // 右 JoyCon 按键掩码（面键、右肩键、右摇杆、START）
    constexpr u64 RIGHT_JOYCON_BUTTONS =
        HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y |
        HidNpadButton_R | HidNpadButton_ZR | HidNpadButton_StickR |
        HidNpadButton_Plus;

    // 8个可单独配置连发的按键，顺序需与 overlay 端 turbo_config.cpp 里的顺序完全一致
    constexpr u64 CHANNEL_MASKS[8] = {
        HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y,
        HidNpadButton_L, HidNpadButton_R, HidNpadButton_ZL, HidNpadButton_ZR
    };
    constexpr const char* CHANNEL_INI_KEYS[8] = {
        "level_a", "level_b", "level_x", "level_y",
        "level_l", "level_r", "level_zl", "level_zr"
    };

    // 速度档位(0=关闭,1=极速,2=高速,3=普通) 对应的按下/松开时长(毫秒)
    // 数值需要和 overlay 端 turbo_config.cpp 的 SPEED_LEVELS 表保持一致
    void LevelToMs(int level, int& press_ms, int& release_ms) {
        switch (level) {
            case 1: press_ms = 40;  release_ms = 40;  break; // 极速
            case 2: press_ms = 70;  release_ms = 70;  break; // 高速
            case 3: press_ms = 120; release_ms = 80;  break; // 普通
            default: press_ms = 0; release_ms = 0; break;    // 关闭
        }
    }
}

// 构造函数
Turbo::Turbo(const char* config_path) {
    m_IsActive = false;
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        m_Channels[i].mask = CHANNEL_MASKS[i];
    }
    LoadConfig(config_path);
}

// 加载配置
void Turbo::LoadConfig(const char* config_path) {
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        int level = ini_getl("AUTOFIRE", CHANNEL_INI_KEYS[i], 0, config_path);
        if (level < 0 || level > 3) level = 0;
        int press_ms = 0, release_ms = 0;
        LevelToMs(level, press_ms, release_ms);
        m_Channels[i].press_ns = (u64)press_ms * 1000000ULL;
        m_Channels[i].release_ns = (u64)release_ms * 1000000ULL;
        // 速度参数变化后，正在进行的周期直接清掉，避免用旧的时间基准继续跑
        m_Channels[i].active = false;
        m_Channels[i].pressed_phase = false;
        m_Channels[i].start_time = 0;
        m_Channels[i].initial_press_time = 0;
    }

    // 读取防止误触开关
    m_DelayStart = ini_getbool("AUTOFIRE", "delaystart", 1, config_path);

    // 读取连发总开关键（0=未设置）。仅在切换键变化时才重置开关状态，
    // 这样在overlay里改速度等设置触发重载时，不会把开关状态弄丢
    u64 toggle_mask = (u64)ini_getl("AUTOFIRE", "togglebutton", 0, config_path);
    if (toggle_mask != m_ToggleMask) {
        m_ToggleMask = toggle_mask;
        m_ToggleOn = false;         // 更换切换键后默认关闭，按一下才开启
        m_ToggleWasDown = false;
    }

    m_isJCRightHand = ini_getbool("AUTOFIRE", "IsJCRightHand", 1, "/config/KeyX/config.ini");

    m_IsActive = false;
}

// 获取只允许左边还是右边的手柄联发
bool Turbo::IsJCRightHand() {
    return m_isJCRightHand;
}

// 把所有通道和聚合状态清零
void Turbo::ResetAllChannels() {
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        m_Channels[i].active = false;
        m_Channels[i].pressed_phase = false;
        m_Channels[i].start_time = 0;
        m_Channels[i].initial_press_time = 0;
    }
    m_IsActive = false;
}

void Turbo::TurboFinishing() {
    ResetAllChannels();
}

// 核心函数：处理输入
void Turbo::Process(ProcessResult& result, bool isJoyCon) {
    u64 raw = result.buttons;

    // 手柄类型限制：joycon单手模式下，只处理对应那一半的按键
    u64 handMask = ~0ULL;
    if (isJoyCon) handMask = m_isJCRightHand ? RIGHT_JOYCON_BUTTONS : LEFT_JOYCON_BUTTONS;

    // 连发总开关键：检测"刚按下"的瞬间，每按一次切换一次开/关
    if (m_ToggleMask != 0) {
        bool toggleDown = (raw & m_ToggleMask) == m_ToggleMask;
        if (toggleDown && !m_ToggleWasDown) {
            m_ToggleOn = !m_ToggleOn;
        }
        m_ToggleWasDown = toggleDown;
    }

    bool masterOff = (m_ToggleMask != 0 && !m_ToggleOn);
    if (masterOff) {
        // 总开关关闭：所有通道立即释放，直接透传真实按键
        bool wasActive = m_IsActive;
        ResetAllChannels();
        if (wasActive) {
            result.event = FeatureEvent::FINISHING;
            result.OtherButtons = raw;
        } else {
            result.event = FeatureEvent::IDLE;
        }
        return;
    }

    u64 output = raw;
    bool anyActive = false;

    for (int i = 0; i < CHANNEL_COUNT; i++) {
        Channel& c = m_Channels[i];
        if (c.press_ns == 0) continue;                 // 该键关闭了连发
        if ((c.mask & handMask) == 0) continue;         // 不属于当前手柄这一半
        if (m_ToggleMask != 0 && (c.mask & m_ToggleMask)) continue; // 开关键本身不参与连发

        bool held = (raw & c.mask) != 0;

        if (!held) {
            c.active = false;
            c.pressed_phase = false;
            c.initial_press_time = 0;
            continue;
        }

        if (!c.active) {
            // 刚检测到按下：先做防误触延迟判断
            if (m_DelayStart) {
                if (c.initial_press_time == 0) c.initial_press_time = armGetSystemTick();
                u64 held_ns = armTicksToNs(armGetSystemTick() - c.initial_press_time);
                if (held_ns < 200000000ULL) continue; // 延迟期间不算激活，也不改输出
            }
            c.active = true;
            c.pressed_phase = true;
            c.start_time = armGetSystemTick();
            c.initial_press_time = 0;
        }

        // 按周期计算当前应处于按下还是松开相位
        u64 elapsed_ns = armTicksToNs(armGetSystemTick() - c.start_time);
        u64 cycle_ns = c.press_ns + c.release_ns;
        u64 pos_in_cycle = elapsed_ns % cycle_ns;
        c.pressed_phase = (pos_in_cycle < c.press_ns);

        if (c.pressed_phase) output |= c.mask;
        else output &= ~c.mask;

        anyActive = true;
    }

    if (!anyActive) {
        if (m_IsActive) {
            m_IsActive = false;
            result.event = FeatureEvent::FINISHING;
            result.OtherButtons = raw;
            return;
        }
        result.event = FeatureEvent::IDLE;
        return;
    }

    if (!m_IsActive) {
        // 从完全空闲到出现第一个连发通道：先触发一帧 STARTING，让外层刷新HDLS状态表
        m_IsActive = true;
        result.event = FeatureEvent::STARTING;
        return;
    }

    result.event = FeatureEvent::Turbo_EXECUTING;
    result.OtherButtons = output;
}
