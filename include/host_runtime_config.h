#pragma once

#include <cstdint>
#include <string>

struct HostRuntimeConfig
{
	uint32_t Width = 1920;
	uint32_t Height = 1080;
	int32_t FrameRate = 60;
	std::string StartupUrl = "https://google.com/";
	bool ShowHelp = false;

	static HostRuntimeConfig FromArgs(int argc, char* argv[]);
	static void PrintUsage();
};
