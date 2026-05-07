#pragma once

#include <cstdint>
#include <string>

/**
 * @brief Runtime options consumed by CEF host startup.
 */
struct HostRuntimeConfig
{
	/** @brief Initial browser width in pixels. */
	uint32_t Width = 1920;
	/** @brief Initial browser height in pixels. */
	uint32_t Height = 1080;
	/** @brief Requested windowless frame rate (1..240). */
	int32_t FrameRate = 60;
	/** @brief Startup URL loaded on first browser creation. */
	std::string StartupUrl = "https://google.com/";
	/** @brief Optional unique process/session suffix for shared IPC object names. */
	std::string SessionId;
	/** @brief Enables thread priority/affinity tuning for worker loops. */
	bool EnableThreadTuning = true;
	/** @brief Enables adaptive begin-frame pacing from consumer cadence. */
	bool EnableCadenceFeedback = false;
	/** @brief Requests usage output and immediate process exit. */
	bool ShowHelp = false;

	/**
	 * @brief Parses runtime config from command-line arguments.
	 * @param argc Argument count from main.
	 * @param argv Argument values from main.
	 * @return Parsed and sanitized runtime config.
	 */
	static HostRuntimeConfig FromArgs(int argc, char* argv[]);
	/** @brief Prints supported command-line options. */
	static void PrintUsage();
};
