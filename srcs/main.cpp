#include "ft_vox.hpp"
#include "StoneEngine.hpp"
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#if defined(__linux__)
struct GraphicsDevice
{
	std::string vendor;
	std::string pciAddress;
	bool isBootVga = false;
};

static std::vector<GraphicsDevice> detectGraphicsDevices()
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
		std::string pciAddress;
		if (!pathError)
			pciAddress = devicePath.filename().string();

		devices.push_back({vendor, pciAddress, bootVga == 1});
	}
	return devices;
}

static std::string makeDriPrimePciSelector(std::string pciAddress)
{
	if (pciAddress.empty())
		return {};
	std::replace(pciAddress.begin(), pciAddress.end(), ':', '_');
	std::replace(pciAddress.begin(), pciAddress.end(), '.', '_');
	return "pci-" + pciAddress;
}

static void configureDefaultGpuPreference()
{
	// OpenGL/GLFW does not expose Vulkan-style physical-device selection.
	// GLVND/NVIDIA reads these variables when GLFW creates the context, so set
	// defaults before glfwInit().  Preserve every explicit user GPU choice.
	if (std::getenv("__NV_PRIME_RENDER_OFFLOAD") ||
		std::getenv("__GLX_VENDOR_LIBRARY_NAME") || std::getenv("DRI_PRIME"))
		return;

	const std::vector<GraphicsDevice> devices = detectGraphicsDevices();
	if (devices.size() < 2)
		return;

	// Prefer a non-primary NVIDIA GPU when present.  GLVND performs NVIDIA's
	// PRIME offload selection during context creation.
	const auto nvidia = std::find_if(devices.begin(), devices.end(),
		[](const GraphicsDevice &device) {
			return device.vendor == "0x10de" && !device.isBootVga;
		});
	if (nvidia != devices.end())
	{
		setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
		setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
		return;
	}

	// Mesa selects AMD render-offload devices through DRI_PRIME.  Use the PCI
	// address instead of a fragile numeric index such as DRI_PRIME=1.
	const auto amd = std::find_if(devices.begin(), devices.end(),
		[](const GraphicsDevice &device) {
			return device.vendor == "0x1002" && !device.isBootVga;
		});
	if (amd != devices.end())
	{
		const std::string selector = makeDriPrimePciSelector(amd->pciAddress);
		if (!selector.empty())
			setenv("DRI_PRIME", selector.c_str(), 0);
	}
}
#else
static void configureDefaultGpuPreference() {}
#endif

bool isWSL() {
	return (std::getenv("WSL_DISTRO_NAME") != nullptr); // WSL_DISTRO_NAME is set in WSL
}

int main(int argc, char **argv)
{
	configureDefaultGpuPreference();

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
