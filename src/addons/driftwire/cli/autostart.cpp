// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// autostart.h's texts: pure, built and tested on every OS.
#include "addons/driftwire/cli/autostart.h"

namespace mv::driftwire::cli {

namespace {

std::string xml_text(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c;
    }
  }
  return out;
}

// One word of ExecStart: double-quoted, with systemd's own specifiers (%) and
// variables ($) taken as written.
std::string systemd_word(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '%') {
      out += "%%";
    } else if (c == '$') {
      out += "$$";
    } else {
      out += c;
    }
  }
  return out + "\"";
}

// One argument as CommandLineToArgvW reads it back.
std::string windows_word(const std::string& s) {
  if (!s.empty() && s.find_first_of(" \t\n\v\"") == std::string::npos) return s;
  std::string out = "\"";
  std::size_t slashes = 0;
  for (const char c : s) {
    if (c == '\\') {
      ++slashes;
      continue;
    }
    if (c == '"') {
      out.append(slashes * 2 + 1, '\\');
    } else {
      out.append(slashes, '\\');
    }
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, '\\');
  return out + "\"";
}

}  // namespace

std::string stable_program(const std::string& exe, const std::function<bool(const std::string&)>& exists) {
  // <prefix>/Cellar/driftwire/<version>/bin/driftwire -> <prefix>/opt/driftwire/bin/driftwire
  const std::string cellar = "/Cellar/driftwire/";
  const auto at = exe.find(cellar);
  if (at == std::string::npos) return exe;
  const auto bin = exe.find("/bin/", at + cellar.size());
  if (bin == std::string::npos) return exe;
  const std::string opt = exe.substr(0, at) + "/opt/driftwire" + exe.substr(bin);
  return exists && exists(opt) ? opt : exe;
}

std::string launch_agent_plist(const autostart_command& c) {
  std::string out =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n"
      "<dict>\n"
      "  <key>Label</key>\n"
      "  <string>";
  out += kAutostartLabel;
  out += "</string>\n  <key>ProgramArguments</key>\n  <array>\n";
  out += "    <string>" + xml_text(c.program) + "</string>\n";
  for (const auto& a : c.args) out += "    <string>" + xml_text(a) + "</string>\n";
  out +=
      "  </array>\n"
      "  <key>RunAtLoad</key>\n"
      "  <true/>\n"
      "  <!-- started again unless it exited 0 (stopped): a crash, or 75 after an update -->\n"
      "  <key>KeepAlive</key>\n"
      "  <dict>\n"
      "    <key>SuccessfulExit</key>\n"
      "    <false/>\n"
      "  </dict>\n"
      "  <key>ThrottleInterval</key>\n"
      "  <integer>30</integer>\n"
      "</dict>\n"
      "</plist>\n";
  return out;
}

std::string systemd_user_unit(const autostart_command& c) {
  std::string exec = systemd_word(c.program);
  for (const auto& a : c.args) exec += " " + systemd_word(a);
  return "[Unit]\n"
         "Description=Driftwire host: the service and relay for your phones and laptop\n"
         "\n"
         "[Service]\n"
         "ExecStart=" + exec + "\n"
         "WorkingDirectory=%h\n"
         "# Started again unless it exited 0 (stopped): a crash, or 75 after an update.\n"
         "Restart=on-failure\n"
         "RestartSec=30\n"
         "# 3: it cannot run as set up (its folder, its keys); trying again would not help.\n"
         "RestartPreventExitStatus=3\n"
         "\n"
         "[Install]\n"
         "WantedBy=default.target\n";
}

std::string windows_command_line(const std::vector<std::string>& argv) {
  std::string out;
  for (const auto& a : argv) {
    if (!out.empty()) out += ' ';
    out += windows_word(a);
  }
  return out;
}

std::string scheduled_task_xml(const autostart_command& c, const std::string& user) {
  const std::string who = user.empty() ? std::string() : "      <UserId>" + xml_text(user) + "</UserId>\n";
  std::string out =
      "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
      "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
      "  <RegistrationInfo>\n"
      "    <Description>Driftwire host: the service and relay for your phones and laptop</Description>\n"
      "  </RegistrationInfo>\n"
      "  <Triggers>\n"
      "    <LogonTrigger>\n"
      "      <Enabled>true</Enabled>\n" +
      who +
      "      <Repetition>\n"
      "        <Interval>PT" + std::to_string(kAutostartRetryMinutes) + "M</Interval>\n"
      "        <StopAtDurationEnd>false</StopAtDurationEnd>\n"
      "      </Repetition>\n"
      "    </LogonTrigger>\n"
      "  </Triggers>\n"
      "  <Principals>\n"
      "    <Principal id=\"Author\">\n" +
      who +
      "      <LogonType>InteractiveToken</LogonType>\n"
      "      <RunLevel>LeastPrivilege</RunLevel>\n"
      "    </Principal>\n"
      "  </Principals>\n"
      "  <Settings>\n"
      "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
      "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
      "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
      "    <AllowHardTerminate>true</AllowHardTerminate>\n"
      "    <StartWhenAvailable>true</StartWhenAvailable>\n"
      "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n"
      "    <IdleSettings>\n"
      "      <StopOnIdleEnd>false</StopOnIdleEnd>\n"
      "      <RestartOnIdle>false</RestartOnIdle>\n"
      "    </IdleSettings>\n"
      "    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
      "    <Enabled>true</Enabled>\n"
      "    <Hidden>true</Hidden>\n"
      "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n"
      "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
      "    <Priority>7</Priority>\n"
      "  </Settings>\n"
      "  <Actions Context=\"Author\">\n"
      "    <Exec>\n"
      "      <Command>" + xml_text(c.program) + "</Command>\n"
      "      <Arguments>" + xml_text(windows_command_line(c.args)) + "</Arguments>\n"
      "    </Exec>\n"
      "  </Actions>\n"
      "</Task>\n";
  return out;
}

std::string linger_hint(const std::string& user) {
  return "To keep it running after a restart before anyone logs in, run once (it needs your password):\n"
         "  sudo loginctl enable-linger " + (user.empty() ? std::string("$USER") : user) + "\n";
}

}  // namespace mv::driftwire::cli
