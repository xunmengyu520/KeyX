#include "turbo_config.hpp"
#include "game.hpp"
#include "ini_helper.hpp"
#include "ipc.hpp"
#include "hiddata.hpp"
#include "refresh.hpp"

namespace {
    // 8个可单独配置的按键槽位，顺序固定：A,B,X,Y,L,R,ZL,ZR
    // 必须和 sys-KeyX/source/autokey/turbo.cpp 里的 CHANNEL_MASKS / CHANNEL_INI_KEYS 顺序完全一致
    struct ButtonSlot {
        const char* displayName;
        const char* iniKey;
        u64 mask;
    };
    constexpr ButtonSlot BUTTON_SLOTS[] = {
        {"A 键",  "level_a",  BTN_A},
        {"B 键",  "level_b",  BTN_B},
        {"X 键",  "level_x",  BTN_X},
        {"Y 键",  "level_y",  BTN_Y},
        {"L 键",  "level_l",  BTN_L},
        {"R 键",  "level_r",  BTN_R},
        {"ZL 键", "level_zl", BTN_ZL},
        {"ZR 键", "level_zr", BTN_ZR},
    };
    constexpr int BUTTON_SLOT_COUNT = sizeof(BUTTON_SLOTS) / sizeof(BUTTON_SLOTS[0]);

    // 速度档位：0=关闭, 1=极速, 2=高速, 3=普通
    // 数值(ms)需要和 sys-KeyX 端 turbo.cpp 的 LevelToMs() 保持一致，这里只用来显示名字/颜色
    struct LevelInfo {
        const char* name;
        tsl::Color color;
    };
    constexpr LevelInfo SPEED_LEVELS[] = {
        {"关闭", {0x8, 0x8, 0x8, 0xF}},                // 灰色
        {"极速", {0xF, 0x5, 0x5, 0xF}},                // 红色
        {"高速", {0x00, 0xDD, 0xFF, 0xFF}},            // 蓝色
        {"普通", {0x00, 0xFF, 0xDD, 0xFF}},            // 标准色(00FFDD)
    };
    constexpr int SPEED_LEVEL_COUNT = sizeof(SPEED_LEVELS) / sizeof(SPEED_LEVELS[0]);

    // 连发开关键可选项：按A依次循环切换
    struct ToggleKeyOption {
        const char* name;
        u64 mask;
    };
    constexpr ToggleKeyOption TOGGLE_KEYS[] = {
        {"关闭", 0},
        {"ZL", BTN_ZL},
        {"ZR", BTN_ZR},
        {"L", BTN_L},
        {"R", BTN_R},
        {"左摇杆按下", BTN_STICKL},
        {"右摇杆按下", BTN_STICKR},
        {"-键", BTN_SELECT},
        {"+键", BTN_START},
    };
    constexpr int TOGGLE_KEY_COUNT = sizeof(TOGGLE_KEYS) / sizeof(TOGGLE_KEYS[0]);

    // 把当前8个按键的等级汇总成一个"是否有连发"的位掩码，
    // 只是为了兼容主菜单预览小黄点的显示逻辑（main_menu.cpp 里读取 AUTOFIRE.buttons）
    void SyncButtonsMaskForPreview(const int levels[], const char* configPath) {
        u64 mask = 0;
        for (int i = 0; i < BUTTON_SLOT_COUNT; i++) {
            if (levels[i] != 0) mask |= BUTTON_SLOTS[i].mask;
        }
        IniHelper::setInt("AUTOFIRE", "buttons", static_cast<int>(mask), configPath);
    }
}

SettingTurboConfig::SettingTurboConfig(bool isGlobal, u64 currentTitleId)  
    : m_isGlobal(isGlobal)
{
    if (!m_isGlobal) {
        GameMonitor::getTitleIdGameName(currentTitleId, m_gameName);
        snprintf(m_ConfigPath, sizeof(m_ConfigPath), "/config/KeyX/GameConfig/%016lX.ini", currentTitleId);
    }
    else {
        snprintf(m_ConfigPath, sizeof(m_ConfigPath), "/config/KeyX/config.ini");
    }

    for (int i = 0; i < BUTTON_SLOT_COUNT; i++) {
        int level = IniHelper::getInt("AUTOFIRE", BUTTON_SLOTS[i].iniKey, 0, m_ConfigPath);
        if (level < 0 || level >= SPEED_LEVEL_COUNT) level = 0;
        m_Levels[i] = level;
    }

    u64 toggleMask = static_cast<u64>(IniHelper::getInt("AUTOFIRE", "togglebutton", 0, m_ConfigPath));
    m_ToggleIdx = 0;
    for (int i = 0; i < TOGGLE_KEY_COUNT; i++) {
        if (TOGGLE_KEYS[i].mask == toggleMask) { m_ToggleIdx = i; break; }
    }

    m_DelayStart = IniHelper::getInt("AUTOFIRE", "delaystart", 1, m_ConfigPath);
}

tsl::elm::Element* SettingTurboConfig::createUI() {
    const char* title = m_isGlobal ? "全局配置" : "独立配置";
    const char* subtitle = m_isGlobal ? "设置全局默认连发参数" : m_gameName;

    auto frame = new tsl::elm::OverlayFrame(title, subtitle);
    auto list = new tsl::elm::List();

    list->addItem(new tsl::elm::CategoryHeader(" 逐键设置连发速度（关闭/极速/高速/普通）"));

    for (int i = 0; i < BUTTON_SLOT_COUNT; i++) {
        const auto& slot = BUTTON_SLOTS[i];
        auto& lvl = SPEED_LEVELS[m_Levels[i]];
        auto item = new tsl::elm::ListItem(slot.displayName, lvl.name);
        item->setValueColor(lvl.color);
        item->setClickListener([this, item, i](u64 keys) {
            if (keys & HidNpadButton_A) {
                m_Levels[i] = (m_Levels[i] + 1) % SPEED_LEVEL_COUNT;
                IniHelper::setInt("AUTOFIRE", BUTTON_SLOTS[i].iniKey, m_Levels[i], m_ConfigPath);
                SyncButtonsMaskForPreview(m_Levels, m_ConfigPath);
                Refresh::RefrRequest(Refresh::MainMenu);
                g_ipcManager.sendReloadAutoFireCommand();
                auto& newLvl = SPEED_LEVELS[m_Levels[i]];
                item->setValue(newLvl.name);
                item->setValueColor(newLvl.color);
                return true;
            }
            return false;
        });
        list->addItem(item);
    }

    list->addItem(new tsl::elm::CategoryHeader(" 连发总开关"));

    auto listItemToggleKey = new tsl::elm::ListItem("连发开关键", TOGGLE_KEYS[m_ToggleIdx].name);
    listItemToggleKey->setClickListener([listItemToggleKey, this](u64 keys) {
        if (keys & HidNpadButton_A) {
            m_ToggleIdx = (m_ToggleIdx + 1) % TOGGLE_KEY_COUNT;
            IniHelper::setInt("AUTOFIRE", "togglebutton", static_cast<int>(TOGGLE_KEYS[m_ToggleIdx].mask), m_ConfigPath);
            g_ipcManager.sendReloadAutoFireCommand();
            listItemToggleKey->setValue(TOGGLE_KEYS[m_ToggleIdx].name);
            return true;
        }
        return false;
    });
    list->addItem(listItemToggleKey);

    list->addItem(new tsl::elm::CategoryHeader(" 延迟启动连发功能避免误触"));

    auto listItemDelayStart = new tsl::elm::ListItem("防止误触", m_DelayStart ? "开" : "关");
    listItemDelayStart->setClickListener([listItemDelayStart, this](u64 keys) {
        if (keys & HidNpadButton_A) {
            m_DelayStart = !m_DelayStart;
            IniHelper::setInt("AUTOFIRE", "delaystart", m_DelayStart ? 1 : 0, m_ConfigPath);
            g_ipcManager.sendReloadAutoFireCommand();
            listItemDelayStart->setValue(m_DelayStart ? "开" : "关");
            return true;
        }
        return false;
    });
    list->addItem(listItemDelayStart);

    frame->setContent(list);
    return frame;
}
