#include "host_runtime_config.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace
{
	bool ParseUInt(const std::string& s, uint32_t& out)
	{
		if (s.empty())
			return false;
		char* end = nullptr;
		unsigned long v = std::strtoul(s.c_str(), &end, 10);
		if (!end || *end != '\0')
			return false;
		out = static_cast<uint32_t>(v);
		return true;
	}

	bool ParseInt(const std::string& s, int32_t& out)
	{
		if (s.empty())
			return false;
		char* end = nullptr;
		long v = std::strtol(s.c_str(), &end, 10);
		if (!end || *end != '\0')
			return false;
		out = static_cast<int32_t>(v);
		return true;
	}
} // namespace

HostRuntimeConfig HostRuntimeConfig::FromArgs(int argc, char* argv[])
{
	HostRuntimeConfig cfg;

	// Accept both '--key value' and '--key=value' forms.
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i] ? argv[i] : "";
		if (arg == "-h" || arg == "--help" || arg == "/?")
		{
			cfg.ShowHelp = true;
			continue;
		}
		if (arg == "--no-thread-tuning")
		{
			cfg.EnableThreadTuning = false;
			continue;
		}
		if (arg == "--enable-cadence-feedback")
		{
			cfg.EnableCadenceFeedback = true;
			continue;
		}

		auto split = arg.find('=');
		auto key = (split == std::string::npos) ? arg : arg.substr(0, split);
		auto value = (split == std::string::npos) ? "" : arg.substr(split + 1);

		auto takeNext = [&](std::string& dst) {
			if (!value.empty())
			{
				dst = value;
				return true;
			}
			if (i + 1 >= argc || !argv[i + 1])
				return false;
			dst = argv[++i];
			return true;
		};

		if (key == "--url")
		{
			std::string v;
			if (takeNext(v) && !v.empty())
				cfg.StartupUrl = v;
			continue;
		}
		if (key == "--width")
		{
			std::string v;
			uint32_t n = 0;
			if (takeNext(v) && ParseUInt(v, n))
				cfg.Width = std::max<uint32_t>(1, n);
			continue;
		}
		if (key == "--height")
		{
			std::string v;
			uint32_t n = 0;
			if (takeNext(v) && ParseUInt(v, n))
				cfg.Height = std::max<uint32_t>(1, n);
			continue;
		}
		if (key == "--fps")
		{
			std::string v;
			int32_t n = 0;
			if (takeNext(v) && ParseInt(v, n))
				cfg.FrameRate = std::clamp(n, 1, 240);
			continue;
		}
		if (key == "--size")
		{
			std::string v;
			if (!takeNext(v))
				continue;
			auto x = v.find_first_of("xX");
			if (x == std::string::npos)
				continue;
			uint32_t w = 0, h = 0;
			if (ParseUInt(v.substr(0, x), w) && ParseUInt(v.substr(x + 1), h))
			{
				cfg.Width = std::max<uint32_t>(1, w);
				cfg.Height = std::max<uint32_t>(1, h);
			}
			continue;
		}
	}

	return cfg;
}

void HostRuntimeConfig::PrintUsage()
{
	std::cout
		<< "CEF Host runtime options:\n"
		<< "  --url <value>                 Startup URL\n"
		<< "  --size <width>x<height>       Browser size\n"
		<< "  --width <value>               Browser width\n"
		<< "  --height <value>              Browser height\n"
		<< "  --fps <1..240>                Windowless frame rate\n"
		<< "  --no-thread-tuning            Disable thread priority/affinity tuning\n"
		<< "  --enable-cadence-feedback     Enable consumer-cadence adaptive pacing\n"
		<< "  -h, --help, /?                Show help\n";
}
