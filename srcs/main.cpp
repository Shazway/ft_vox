#include "ft_vox.hpp"
#include "StoneEngine.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
struct GraphicsDevice
{
	std::string vendor;
	std::string pciAddress;
	bool isBootVga = false;
};

void glfwErrorCallback(int error, const char *description)
{
	std::cerr << "GLFW error " << error << ": "
		<< (description ? description : "unknown error") << std::endl;
}

#if defined(__linux__)
std::vector<GraphicsDevice> detectGraphicsDevices()
{
	std::vector<GraphicsDevice> devices;
	std::error_code error;

	for (std::filesystem::directory_iterator entry("/sys/class/drm", error), end;
		 !error && entry != end; entry.increment(error))
	{
		const std::string name = entry->path().filename().string();
		if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos)
			continue;

		std::ifstream vendorFile(entry->path() / "device/vendor");
		std::string vendor;
		if (!(vendorFile >> vendor))
			continue;

		int bootVga = 0;
		std::ifstream(entry->path() / "device/boot_vga") >> bootVga;

		std::error_code pathError;
		const std::filesystem::path devicePath =
			std::filesystem::canonical(entry->path() / "device", pathError);
		devices.push_back({vendor,
			pathError ? std::string() : devicePath.filename().string(),
			bootVga == 1});
	}
	return devices;
}

std::string makeDriPrimePciSelector(std::string pciAddress)
{
	if (pciAddress.empty())
		return {};
	std::replace(pciAddress.begin(), pciAddress.end(), ':', '_');
	std::replace(pciAddress.begin(), pciAddress.end(), '.', '_');
	return "pci-" + pciAddress;
}

void configureDefaultGpuPreference()
{
	// An explicit user choice always wins, including DRI_PRIME=0.
	if (std::getenv("DRI_PRIME") || std::getenv("__NV_PRIME_RENDER_OFFLOAD")
		|| std::getenv("__GLX_VENDOR_LIBRARY_NAME"))
	{
		std::cout << "GPU preference: using environment override" << std::endl;
		return;
	}

	const std::vector<GraphicsDevice> devices = detectGraphicsDevices();
	if (devices.size() < 2)
		return;

	// NVIDIA's EGL path only needs PRIME_RENDER_OFFLOAD; GLX additionally
	// needs the GLVND vendor name.  Set both because GLFW may select either.
	const auto nvidia = std::find_if(devices.begin(), devices.end(),
		[](const GraphicsDevice &device) {
			return device.vendor == "0x10de" && !device.isBootVga;
		});
	if (nvidia != devices.end())
	{
		setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
		setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
		std::cout << "GPU preference: NVIDIA " << nvidia->pciAddress << std::endl;
		return;
	}

	// Mesa accepts a stable PCI selector for AMD render-offload devices.
	const auto amd = std::find_if(devices.begin(), devices.end(),
		[](const GraphicsDevice &device) {
			return device.vendor == "0x1002" && !device.isBootVga;
		});
	if (amd != devices.end())
	{
		const std::string selector = makeDriPrimePciSelector(amd->pciAddress);
		if (!selector.empty())
		{
			setenv("DRI_PRIME", selector.c_str(), 0);
			std::cout << "GPU preference: AMD " << amd->pciAddress << std::endl;
		}
	}
}
#else
void configureDefaultGpuPreference() {}
#endif
}

bool isWSL() {
	return (std::getenv("WSL_DISTRO_NAME") != nullptr); // WSL_DISTRO_NAME is set in WSL
}

int main(int argc, char **argv)
{
	configureDefaultGpuPreference();
	glfwSetErrorCallback(glfwErrorCallback);

	int seed = 42;
	if (argc == 2)
	{
		seed = atoi(argv[1]);
	}
	// Under sanitizers, Mesa's driver threads can trigger benign data race
	// reports. Disabling Mesa's multi-threaded GL dispatch reduces noise.
	// This environment hint is harmless if unsupported.
	setenv("MESA_GLTHREAD", "0", 1);
	if (!glfwInit())
	{
		std::cerr << "Failed to initialize GLFW" << std::endl;
		return -1;
	}
	ThreadPool pool(std::thread::hardware_concurrency());
	StoneEngine stone(seed, pool);
	if (!stone.isInitialized())
	{
		pool.joinThreads();
		return EXIT_FAILURE;
	}
	stone.run();
	// Ensure all worker threads are stopped before tearing down world/GL
	pool.joinThreads();
	return 0;
}
