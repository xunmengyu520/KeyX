#pragma once

#include <switch.h>
#include "common.hpp"

class Turbo {
public:
    Turbo(const char* config_path);

    // 加载配置
    void LoadConfig(const char* config_path);

    // 核心函数：处理输入，填充处理结果（事件+按键数据）
    void Process(ProcessResult& result, bool isJoyCon);

    // 重置连发状态（用于暂停时清理）
    void TurboFinishing();

    // 获取只允许左边还是右边的手柄联发
    bool IsJCRightHand();

private:
    bool m_isJCRightHand = true;

    // 每个可单独配置的按键，都是一条独立的连发通道
    struct Channel {
        u64 mask = 0;               // 按键掩码
        u64 press_ns = 0;           // 按下时长（纳秒），0 表示该键关闭连发
        u64 release_ns = 0;         // 松开时长（纳秒）
        bool active = false;        // 当前是否正在连发周期中
        bool pressed_phase = false; // 当前处于按下相位还是松开相位
        u64 start_time = 0;         // 本次连发周期起始时间
        u64 initial_press_time = 0; // 用于防误触延迟启动判定
    };
    static constexpr int CHANNEL_COUNT = 8; // A,B,X,Y,L,R,ZL,ZR
    Channel m_Channels[CHANNEL_COUNT];

    bool m_DelayStart;          // 是否启用延迟启动（防误触）
    bool m_IsActive;            // 聚合状态：是否有任意通道正在连发中

    // 连发总开关（切换键）：按一下开启全部连发，再按一下全部关闭
    u64  m_ToggleMask = 0;
    bool m_ToggleOn = false;
    bool m_ToggleWasDown = false;

    // 把所有通道和聚合状态清零
    void ResetAllChannels();
};
