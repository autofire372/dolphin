// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <tuple>

#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/UdpSocket.hpp>
#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Random.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/CoreTiming.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPProto.h"

namespace ciface::DualShockUDPClient
{
constexpr std::string_view DUALSHOCKUDP_SOURCE_NAME = "DSUClient";

namespace Settings
{
const Config::Info<std::string> SERVER_ADDRESS{
    {Config::System::DualShockUDPClient, "Server", "IPAddress"}, ""};
const Config::Info<int> SERVER_PORT{{Config::System::DualShockUDPClient, "Server", "Port"}, 0};
const Config::Info<std::string> SERVERS{{Config::System::DualShockUDPClient, "Server", "Entries"},
                                        ""};
const Config::Info<bool> SERVERS_ENABLED{{Config::System::DualShockUDPClient, "Server", "Enabled"},
                                         false};
}  // namespace Settings

// Clock type used for querying timeframes
using SteadyClock = std::chrono::steady_clock;

class Device final : public Core::Device
{
private:
  template <class T>
  class Button final : public Input
  {
  public:
    Button(const char* name, const T& buttons, T mask)
        : m_name(name), m_buttons(buttons), m_mask(mask)
    {
    }
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override { return (m_buttons & m_mask) != 0; }

  private:
    const char* const m_name;
    const T& m_buttons;
    const T m_mask;
  };

  template <class T>
  class AnalogInput : public Input
  {
  public:
    AnalogInput(const char* name, const T& input, ControlState range, ControlState offset = 0)
        : m_name(name), m_input(input), m_range(range), m_offset(offset)
    {
    }
    std::string GetName() const final override { return m_name; }
    ControlState GetState() const final override
    {
      return (ControlState(m_input) + m_offset) / m_range;
    }

  private:
    const char* m_name;
    const T& m_input;
    const ControlState m_range;
    const ControlState m_offset;
  };

  class TouchInput final : public AnalogInput<int>
  {
  public:
    using AnalogInput::AnalogInput;
    bool IsDetectable() const override { return false; }
  };

  class MotionInput final : public AnalogInput<float>
  {
  public:
    using AnalogInput::AnalogInput;
    bool IsDetectable() const override { return false; }
  };

  using AccelerometerInput = MotionInput;
  using GyroInput = MotionInput;

  class BatteryInput final : public Input
  {
  public:
    using BatteryState = Proto::DsBattery;

    BatteryInput(const BatteryState& battery) : m_battery(battery) {}

    std::string GetName() const override { return "Battery"; }

    ControlState GetState() const override
    {
      switch (m_battery)
      {
      case BatteryState::Charging:  // We don't actually know the battery level in this case
      case BatteryState::Charged:
        return BATTERY_INPUT_MAX_VALUE;
      default:
        return ControlState(m_battery) / ControlState(BatteryState::Full) * BATTERY_INPUT_MAX_VALUE;
      }
    }

    bool IsDetectable() const override { return false; }

  private:
    const BatteryState& m_battery;
  };

public:
  void UpdateInput() override;

  Device(std::string name, int index, std::string server_address, u16 server_port);

  std::string GetName() const final override;
  std::string GetSource() const final override;
  std::optional<int> GetPreferredId() const final override;

private:
  void ResetPadData();

  const std::string m_name;
  const int m_index;
  sf::UdpSocket m_socket;
  SteadyClock::time_point m_next_reregister = SteadyClock::time_point::min();
  Proto::MessageType::PadDataResponse m_pad_data{};
  Proto::Touch m_prev_touch{};
  bool m_prev_touch_valid = false;
  int m_touch_x = 0;
  int m_touch_y = 0;
  std::string m_server_address;
  u16 m_server_port;

  s16 m_touch_x_min;
  s16 m_touch_y_min;
  s16 m_touch_x_max;
  s16 m_touch_y_max;
};

using MathUtil::GRAVITY_ACCELERATION;
constexpr auto SERVER_REREGISTER_INTERVAL = std::chrono::seconds{1};
constexpr auto SERVER_LISTPORTS_INTERVAL = std::chrono::seconds{1};
constexpr int TOUCH_X_AXIS_MAX = 1000;
constexpr int TOUCH_Y_AXIS_MAX = 500;
constexpr auto THREAD_MAX_WAIT_INTERVAL = std::chrono::milliseconds{250};
constexpr auto SERVER_UNRESPONSIVE_INTERVAL = std::chrono::seconds{1};  // Can be 0
constexpr u32 SERVER_ASKED_PADS = 4;

struct Server
{
  Server(std::string description, std::string address, u16 port)
      : m_description{std::move(description)}, m_address{std::move(address)}, m_port{port}
  {
  }
  Server(const Server&) = delete;
  Server(Server&& other) noexcept
  {
    m_description = std::move(other.m_description);
    m_address = std::move(other.m_address);
    m_port = other.m_port;
    m_port_info = std::move(other.m_port_info);
  }

  Server& operator=(const Server&) = delete;
  Server& operator=(Server&&) = delete;

  ~Server() = default;

  std::string m_description;
  std::string m_address;
  u16 m_port;
  std::mutex m_port_info_mutex;
  std::array<Proto::MessageType::PortInfo, Proto::PORT_COUNT> m_port_info;
  sf::UdpSocket m_socket;
  SteadyClock::time_point m_disconnect_time = SteadyClock::now();
};

static bool s_servers_enabled;
static std::vector<Server> s_servers;
static u32 s_client_uid;
static SteadyClock::time_point s_next_listports_time;
static std::thread s_hotplug_thread;
static Common::Flag s_hotplug_thread_running;

static bool IsSameController(const Proto::MessageType::PortInfo& a,
                             const Proto::MessageType::PortInfo& b)
{
  // compare everything but battery_status
  return std::tie(a.pad_id, a.pad_state, a.model, a.connection_type, a.pad_mac_address) ==
         std::tie(b.pad_id, b.pad_state, b.model, b.connection_type, b.pad_mac_address);
}

static void HotplugThreadFunc()
{
  Common::SetCurrentThreadName("DualShockUDPClient Hotplug Thread");
  INFO_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient hotplug thread started");

  std::vector<bool> timed_out_servers(s_servers.size(), false);

  while (s_hotplug_thread_running.IsSet())
  {
    using namespace std::chrono;
    using namespace std::chrono_literals;

    const auto now = SteadyClock::now();
    if (now >= s_next_listports_time)
    {
      s_next_listports_time = now + SERVER_LISTPORTS_INTERVAL;

      for (size_t i = 0; i < s_servers.size(); ++i)
      {
        auto& server = s_servers[i];
        Proto::Message<Proto::MessageType::ListPorts> msg(s_client_uid);
        auto& list_ports = msg.m_message;
        // We ask for x possible devices. We will receive a message for every connected device.
        list_ports.pad_request_count = SERVER_ASKED_PADS;
        list_ports.pad_ids = {0, 1, 2, 3};
        msg.Finish();
        if (server.m_socket.send(&list_ports, sizeof list_ports, server.m_address, server.m_port) !=
            sf::Socket::Status::Done)
        {
          ERROR_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient HotplugThreadFunc send failed");
        }
        timed_out_servers[i] = true;
      }
    }

    sf::SocketSelector selector;
    for (auto& server : s_servers)
    {
      selector.add(server.m_socket);
    }

    auto timeout = duration_cast<milliseconds>(s_next_listports_time - SteadyClock::now());

    // Receive controller port info within a time from our request.
    // Run this even if we sent no new requests, to disconnect devices,
    // sleep (wait) the thread and catch old responses.
    do
    {
      // Selector's wait treats a timeout of zero as infinite timeout, which we don't want,
      // but we also don't want risk waiting for the whole SERVER_LISTPORTS_INTERVAL and hang
      // the thead trying to close this one in case we received no answers.
      const auto current_timeout = std::max(std::min(timeout, THREAD_MAX_WAIT_INTERVAL), 1ms);
      timeout -= current_timeout;
      // This will return at the first answer
      if (selector.wait(sf::milliseconds(current_timeout.count())))
      {
        // Now check all the servers because we don't know which one(s) sent a reply
        for (size_t i = 0; i < s_servers.size(); ++i)
        {
          auto& server = s_servers[i];
          if (!selector.isReady(server.m_socket))
          {
            continue;
          }

          Proto::Message<Proto::MessageType::FromServer> msg;
          std::size_t received_bytes;
          sf::IpAddress sender;
          u16 port;
          if (server.m_socket.receive(&msg, sizeof(msg), received_bytes, sender, port) !=
              sf::Socket::Status::Done)
          {
            continue;
          }

          if (auto port_info = msg.CheckAndCastTo<Proto::MessageType::PortInfo>())
          {
            server.m_disconnect_time = SteadyClock::now() + SERVER_UNRESPONSIVE_INTERVAL;
            // We have receive at least one valid update, that's enough. This is needed to avoid
            // false positive when checking for disconnection in case our thread waited too long
            timed_out_servers[i] = false;

            const bool port_changed =
                !IsSameController(*port_info, server.m_port_info[port_info->pad_id]);
            if (port_changed)
            {
              server.m_port_info[port_info->pad_id] = *port_info;
              // Just remove and re-add all the devices for simplicity
              g_controller_interface.PlatformPopulateDevices([] { PopulateDevices(); });
            }
          }
        }
      }
      if (!s_hotplug_thread_running.IsSet())  // Avoid hanging the thread for too long
        return;
    } while (timeout > 0ms);

    // If we have failed to receive any information from the server (or even send it),
    // disconnect all devices from it (after enough time has elapsed, to avoid false positives).
    for (size_t i = 0; i < s_servers.size(); ++i)
    {
      auto& server = s_servers[i];
      if (timed_out_servers[i] && SteadyClock::now() >= server.m_disconnect_time)
      {
        bool any_connected = false;
        for (size_t port_index = 0; port_index < server.m_port_info.size(); port_index++)
        {
          any_connected = any_connected ||
                          server.m_port_info[port_index].pad_state == Proto::DsState::Connected;
          server.m_port_info[port_index] = {};
          server.m_port_info[port_index].pad_id = static_cast<u8>(port_index);
        }
        // We can't only remove devices added by this server as we wouldn't know which they are
        if (any_connected)
          g_controller_interface.PlatformPopulateDevices([] { PopulateDevices(); });
      }
    }
  }
  INFO_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient hotplug thread stopped");
}

static void StartHotplugThread()
{
  // Mark the thread as running.
  if (!s_hotplug_thread_running.TestAndSet())
  {
    // It was already running.
    return;
  }

  s_hotplug_thread = std::thread(HotplugThreadFunc);
}

static void StopHotplugThread()
{
  // Tell the hotplug thread to stop.
  if (!s_hotplug_thread_running.TestAndClear())
  {
    // It wasn't running, we're done.
    return;
  }

  for (auto& server : s_servers)
  {
    server.m_socket.unbind();  // interrupt blocking socket
  }
  s_hotplug_thread.join();
}

static void Restart()
{
  INFO_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient Restart");

  StopHotplugThread();

  for (auto& server : s_servers)
  {
    for (size_t port_index = 0; port_index < server.m_port_info.size(); port_index++)
    {
      server.m_port_info[port_index] = {};
      server.m_port_info[port_index].pad_id = static_cast<u8>(port_index);
    }
  }

  PopulateDevices();  // Only removes devices

  s_client_uid = Common::Random::GenerateValue<u32>();
  s_next_listports_time = SteadyClock::now();

  if (s_servers_enabled && !s_servers.empty())
    StartHotplugThread();
}

static void ConfigChanged()
{
  const bool servers_enabled = Config::Get(Settings::SERVERS_ENABLED);
  const std::string servers_setting = Config::Get(Settings::SERVERS);

  std::string new_servers_setting;
  for (const auto& server : s_servers)
  {
    new_servers_setting +=
        fmt::format("{}:{}:{};", server.m_description, server.m_address, server.m_port);
  }

  if (servers_enabled != s_servers_enabled || servers_setting != new_servers_setting)
  {
    s_servers_enabled = servers_enabled;
    s_servers.clear();

    const auto server_details = SplitString(servers_setting, ';');
    for (const auto& server_detail : server_details)
    {
      const auto server_info = SplitString(server_detail, ':');
      if (server_info.size() < 3)
        continue;

      const std::string description = server_info[0];
      const std::string server_address = server_info[1];
      const auto port = std::stoi(server_info[2]);
      if (port >= std::numeric_limits<u16>::max())
      {
        continue;
      }
      u16 server_port = static_cast<u16>(port);

      s_servers.emplace_back(description, server_address, server_port);
    }
    Restart();
  }
}

void Init()
{
  // The following is added for backwards compatibility
  const auto server_address_setting = Config::Get(Settings::SERVER_ADDRESS);
  const auto server_port_setting = Config::Get(Settings::SERVER_PORT);

  if (!server_address_setting.empty() && server_port_setting != 0)
  {
    const auto& servers_setting = Config::Get(ciface::DualShockUDPClient::Settings::SERVERS);
    Config::SetBaseOrCurrent(ciface::DualShockUDPClient::Settings::SERVERS,
                             servers_setting + fmt::format("{}:{}:{};", "DS4",
                                                           server_address_setting,
                                                           server_port_setting));
    Config::SetBase(Settings::SERVER_ADDRESS, "");
    Config::SetBase(Settings::SERVER_PORT, 0);
  }

  // It would be much better to unbind from this callback on DeInit but it's not possible as of now
  Config::AddConfigChangedCallback(ConfigChanged);
  ConfigChanged();  // Call it immediately to load settings
}

void PopulateDevices()
{
  INFO_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient PopulateDevices");

  // s_servers has already been updated so we can't use it to know which devices we removed,
  // also it's good to remove all of them before adding new ones so that their id will be set
  // correctly if they have the same name
  g_controller_interface.RemoveDevice(
      [](const auto* dev) { return dev->GetSource() == DUALSHOCKUDP_SOURCE_NAME; });

  for (auto& server : s_servers)
  {
    std::lock_guard lock{server.m_port_info_mutex};
    for (size_t port_index = 0; port_index < server.m_port_info.size(); port_index++)
    {
      const Proto::MessageType::PortInfo& port_info = server.m_port_info[port_index];
      if (port_info.pad_state != Proto::DsState::Connected)
        continue;

      g_controller_interface.AddDevice(std::make_shared<Device>(
          server.m_description, static_cast<int>(port_index), server.m_address, server.m_port));
    }
  }
}

void DeInit()
{
  StopHotplugThread();
}

Device::Device(std::string name, int index, std::string server_address, u16 server_port)
    : m_name{std::move(name)}, m_index{index}, m_server_address{std::move(server_address)},
      m_server_port{server_port}
{
  m_socket.setBlocking(false);

  AddInput(new AnalogInput<u8>("Pad W", m_pad_data.button_dpad_left_analog, 255));
  AddInput(new AnalogInput<u8>("Pad S", m_pad_data.button_dpad_down_analog, 255));
  AddInput(new AnalogInput<u8>("Pad E", m_pad_data.button_dpad_right_analog, 255));
  AddInput(new AnalogInput<u8>("Pad N", m_pad_data.button_dpad_up_analog, 255));
  AddInput(new AnalogInput<u8>("Square", m_pad_data.button_square_analog, 255));
  AddInput(new AnalogInput<u8>("Cross", m_pad_data.button_cross_analog, 255));
  AddInput(new AnalogInput<u8>("Circle", m_pad_data.button_circle_analog, 255));
  AddInput(new AnalogInput<u8>("Triangle", m_pad_data.button_triangle_analog, 255));
  AddInput(new AnalogInput<u8>("L1", m_pad_data.button_l1_analog, 255));
  AddInput(new AnalogInput<u8>("R1", m_pad_data.button_r1_analog, 255));

  AddInput(new AnalogInput<u8>("L2", m_pad_data.trigger_l2, 255));
  AddInput(new AnalogInput<u8>("R2", m_pad_data.trigger_r2, 255));

  AddInput(new Button<u8>("L3", m_pad_data.button_states1, 0x2));
  AddInput(new Button<u8>("R3", m_pad_data.button_states1, 0x4));
  AddInput(new Button<u8>("Share", m_pad_data.button_states1, 0x1));
  AddInput(new Button<u8>("Options", m_pad_data.button_states1, 0x8));
  AddInput(new Button<u8>("PS", m_pad_data.button_ps, 0x1));
  AddInput(new Button<u8>("Touch Button", m_pad_data.button_touch, 0x1));

  AddInput(new AnalogInput<u8>("Left X-", m_pad_data.left_stick_x, -128, -128));
  AddInput(new AnalogInput<u8>("Left X+", m_pad_data.left_stick_x, 127, -128));
  AddInput(new AnalogInput<u8>("Left Y-", m_pad_data.left_stick_y_inverted, -128, -128));
  AddInput(new AnalogInput<u8>("Left Y+", m_pad_data.left_stick_y_inverted, 127, -128));
  AddInput(new AnalogInput<u8>("Right X-", m_pad_data.right_stick_x, -128, -128));
  AddInput(new AnalogInput<u8>("Right X+", m_pad_data.right_stick_x, 127, -128));
  AddInput(new AnalogInput<u8>("Right Y-", m_pad_data.right_stick_y_inverted, -128, -128));
  AddInput(new AnalogInput<u8>("Right Y+", m_pad_data.right_stick_y_inverted, 127, -128));

  AddInput(new TouchInput("Touch X-", m_touch_x, -TOUCH_X_AXIS_MAX));
  AddInput(new TouchInput("Touch X+", m_touch_x, TOUCH_X_AXIS_MAX));
  AddInput(new TouchInput("Touch Y-", m_touch_y, -TOUCH_Y_AXIS_MAX));
  AddInput(new TouchInput("Touch Y+", m_touch_y, TOUCH_Y_AXIS_MAX));

  // Convert Gs to meters per second squared
  constexpr auto accel_scale = 1.0 / GRAVITY_ACCELERATION;

  AddInput(new AccelerometerInput("Accel Up", m_pad_data.accelerometer_y_g, -accel_scale));
  AddInput(new AccelerometerInput("Accel Down", m_pad_data.accelerometer_y_g, accel_scale));
  AddInput(new AccelerometerInput("Accel Left", m_pad_data.accelerometer_x_g, accel_scale));
  AddInput(new AccelerometerInput("Accel Right", m_pad_data.accelerometer_x_g, -accel_scale));
  AddInput(new AccelerometerInput("Accel Forward", m_pad_data.accelerometer_z_g, accel_scale));
  AddInput(new AccelerometerInput("Accel Backward", m_pad_data.accelerometer_z_g, -accel_scale));

  // Convert degrees per second to radians per second
  constexpr auto gyro_scale = 360.0 / MathUtil::TAU;

  AddInput(new GyroInput("Gyro Pitch Up", m_pad_data.gyro_pitch_deg_s, gyro_scale));
  AddInput(new GyroInput("Gyro Pitch Down", m_pad_data.gyro_pitch_deg_s, -gyro_scale));
  AddInput(new GyroInput("Gyro Roll Left", m_pad_data.gyro_roll_deg_s, -gyro_scale));
  AddInput(new GyroInput("Gyro Roll Right", m_pad_data.gyro_roll_deg_s, gyro_scale));
  AddInput(new GyroInput("Gyro Yaw Left", m_pad_data.gyro_yaw_deg_s, -gyro_scale));
  AddInput(new GyroInput("Gyro Yaw Right", m_pad_data.gyro_yaw_deg_s, gyro_scale));

  AddInput(new BatteryInput(m_pad_data.battery_status));

  m_touch_x_min = 0;
  m_touch_y_min = 0;
  // DS4 touchpad max values
  m_touch_x_max = 1919;
  m_touch_y_max = 941;

  ResetPadData();
}

void Device::ResetPadData()
{
  m_pad_data = Proto::MessageType::PadDataResponse{};

  // Make sure they start from resting values, not from 0
  m_touch_x = m_touch_x_min + ((m_touch_x_max - m_touch_x_min) / 2.0);
  m_touch_y = m_touch_y_min + ((m_touch_y_max - m_touch_y_min) / 2.0);
  m_pad_data.left_stick_x = 128;
  m_pad_data.left_stick_y_inverted = 128;
  m_pad_data.right_stick_x = 128;
  m_pad_data.right_stick_y_inverted = 128;
  m_pad_data.touch1.x = m_touch_x;
  m_pad_data.touch1.y = m_touch_y;
}

std::string Device::GetName() const
{
  return m_name;
}

std::string Device::GetSource() const
{
  return std::string(DUALSHOCKUDP_SOURCE_NAME);
}

void Device::UpdateInput()
{
  // Regularly tell the UDP server to feed us controller data
  const auto now = SteadyClock::now();
  if (now >= m_next_reregister)
  {
    m_next_reregister = now + SERVER_REREGISTER_INTERVAL;

    Proto::Message<Proto::MessageType::PadDataRequest> msg(s_client_uid);
    auto& data_req = msg.m_message;
    data_req.register_flags = Proto::RegisterFlags::PadID;
    data_req.pad_id_to_register = m_index;
    msg.Finish();
    if (m_socket.send(&data_req, sizeof(data_req), m_server_address, m_server_port) !=
        sf::Socket::Status::Done)
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient UpdateInput send failed");
    }
  }

  // Receive and handle controller data
  Proto::Message<Proto::MessageType::FromServer> msg;
  std::size_t received_bytes;
  sf::IpAddress sender;
  u16 port;
  while (m_socket.receive(&msg, sizeof msg, received_bytes, sender, port) ==
         sf::Socket::Status::Done)
  {
    if (auto pad_data = msg.CheckAndCastTo<Proto::MessageType::PadDataResponse>())
    {
      m_pad_data = *pad_data;

      // Update touch pad relative coordinates
      if (m_pad_data.touch1.id != m_prev_touch.id)
        m_prev_touch_valid = false;
      if (m_prev_touch_valid)
      {
        m_touch_x += m_pad_data.touch1.x - m_prev_touch.x;
        m_touch_y += m_pad_data.touch1.y - m_prev_touch.y;
        m_touch_x = std::clamp(m_touch_x, -TOUCH_X_AXIS_MAX, TOUCH_X_AXIS_MAX);
        m_touch_y = std::clamp(m_touch_y, -TOUCH_Y_AXIS_MAX, TOUCH_Y_AXIS_MAX);
      }
      m_prev_touch = m_pad_data.touch1;
      m_prev_touch_valid = true;
    }
  }
}

std::optional<int> Device::GetPreferredId() const
{
  return m_index;
}

}  // namespace ciface::DualShockUDPClient
