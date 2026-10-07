/*
 *  Voice Bridge client - renders the in-game menu to BMP files without the
 *  game, to check its layout: menu-preview.exe [output folder]
 */

#include <d3d9.h>
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace vbc::overlay
{
void PreviewFrame(IDirect3DDevice9* device, int page, bool portuguese);
}

namespace
{
bool saveBitmap(IDirect3DDevice9* device, const std::string& path, UINT width, UINT height)
{
	IDirect3DSurface9* back = nullptr;
	IDirect3DSurface9* copy = nullptr;
	if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) ||
		FAILED(device->CreateOffscreenPlainSurface(width, height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr)) ||
		FAILED(device->GetRenderTargetData(back, copy)))
	{
		return false;
	}
	D3DLOCKED_RECT rect {};
	copy->LockRect(&rect, nullptr, D3DLOCK_READONLY);
	BITMAPFILEHEADER file {};
	BITMAPINFOHEADER info {};
	info.biSize = sizeof(info);
	info.biWidth = static_cast<LONG>(width);
	info.biHeight = -static_cast<LONG>(height);
	info.biPlanes = 1;
	info.biBitCount = 32;
	file.bfType = 0x4D42;
	file.bfOffBits = sizeof(file) + sizeof(info);
	file.bfSize = file.bfOffBits + width * height * 4;
	FILE* out = std::fopen(path.c_str(), "wb");
	std::fwrite(&file, sizeof(file), 1, out);
	std::fwrite(&info, sizeof(info), 1, out);
	for (UINT y = 0; y < height; ++y)
	{
		std::fwrite(static_cast<const uint8_t*>(rect.pBits) + y * rect.Pitch, 4, width, out);
	}
	std::fclose(out);
	copy->UnlockRect();
	copy->Release();
	back->Release();
	return true;
}
}

int main(int argc, char** argv)
{
	const std::string folder = argc > 1 ? argv[1] : ".";
	const bool portuguese = !(argc > 2 && std::string(argv[2]) == "en"); // usage: menu-preview [folder] [en]
	const UINT width = 1280;
	const UINT height = 720;
	WNDCLASSA windowClass {};
	windowClass.lpfnWndProc = DefWindowProcA;
	windowClass.hInstance = GetModuleHandleA(nullptr);
	windowClass.lpszClassName = "VoiceBridgePreview";
	RegisterClassA(&windowClass);
	HWND window = CreateWindowA("VoiceBridgePreview", "preview", WS_OVERLAPPEDWINDOW, 0, 0, width, height, nullptr, nullptr, windowClass.hInstance, nullptr);

	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS parameters {};
	parameters.Windowed = TRUE;
	parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
	parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
	parameters.BackBufferWidth = width;
	parameters.BackBufferHeight = height;
	parameters.hDeviceWindow = window;
	IDirect3DDevice9* device = nullptr;
	if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device)))
	{
		std::printf("could not create a Direct3D 9 device\n");
		return 1;
	}

	const char* names[] = { "status", "sound", "microphone", "players", "interface", "about" };
	for (int page = 0; page < 6; ++page)
	{
		for (int frame = 0; frame < 4; ++frame)
		{
			device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(70, 92, 110), 1.f, 0);
			vbc::overlay::PreviewFrame(device, page, portuguese);
		}
		const std::string path = folder + "\\menu-" + names[page] + ".bmp";
		std::printf("%s %s\n", path.c_str(), saveBitmap(device, path, width, height) ? "ok" : "FAILED");
	}
	device->Release();
	d3d->Release();
	return 0;
}
