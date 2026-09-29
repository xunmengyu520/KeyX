#pragma once
#include <tesla.hpp>

class SettingTurboConfig : public tsl::Gui 
{
public:
    SettingTurboConfig(bool isGlobal, u64 currentTitleId);
    virtual tsl::elm::Element* createUI() override;
    
private:
    bool m_isGlobal;      // true=全局配置, false=独立配置
    char m_gameName[64];  // 当前游戏名称
    char m_ConfigPath[64];  // 配置文件路径

    // 8个按键各自的连发速度档位：0=关闭, 1=极速, 2=高速, 3=普通
    // 顺序固定为 A,B,X,Y,L,R,ZL,ZR，需与 turbo_config.cpp 里的 BUTTON_SLOTS 一致
    static constexpr int BUTTON_COUNT = 8;
    int m_Levels[BUTTON_COUNT];

    int m_ToggleIdx;    // 连发开关键在 TOGGLE_KEYS 表中的序号（0=关闭该功能）
    bool m_DelayStart;  // 是否延迟启动（防误触）
};
