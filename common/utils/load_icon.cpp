/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Guillaume Meunier <guillaume.meunier@centraliens.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "load_icon.h"

#include <windows.h>
#include <wincodec.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
template <typename T>
class com_ptr
{
	T * ptr = nullptr;

public:
	com_ptr() = default;
	com_ptr(const com_ptr &) = delete;
	com_ptr & operator=(const com_ptr &) = delete;
	~com_ptr()
	{
		if (ptr)
			ptr->Release();
	}

	T ** operator&() { return &ptr; }
	T * operator->() { return ptr; }
	T * get() const { return ptr; }
	explicit operator bool() const { return ptr != nullptr; }
};

struct com_initializer
{
	com_initializer()
	{
		// The server may already be on an initialized thread; both results are fine.
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	}
	~com_initializer() { CoUninitialize(); }
};

std::wstring widen(const std::filesystem::path & path)
{
	auto utf8 = path.u8string();
	if (utf8.empty())
		return {};

	int size = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char *>(utf8.data()), (int)utf8.size(), nullptr, 0);
	if (size <= 0)
		return {};
	std::wstring result(size, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char *>(utf8.data()), (int)utf8.size(), result.data(), size);
	return result;
}

std::vector<std::byte> encode_png(IWICImagingFactory * factory, IWICBitmapSource * source, UINT width, UINT height)
{
	com_ptr<IStream> stream;
	if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
		throw std::runtime_error("CreateStreamOnHGlobal failed");

	com_ptr<IWICBitmapEncoder> encoder;
	if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
		throw std::runtime_error("cannot create the PNG encoder");
	if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)))
		throw std::runtime_error("cannot initialize the PNG encoder");

	com_ptr<IWICBitmapFrameEncode> frame;
	if (FAILED(encoder->CreateNewFrame(&frame, nullptr)))
		throw std::runtime_error("cannot create a PNG frame");
	if (FAILED(frame->Initialize(nullptr)))
		throw std::runtime_error("cannot initialize the PNG frame");

	frame->SetSize(width, height);
	if (FAILED(frame->WriteSource(source, nullptr)))
		throw std::runtime_error("cannot write the PNG frame");
	if (FAILED(frame->Commit()))
		throw std::runtime_error("cannot commit the PNG frame");
	if (FAILED(encoder->Commit()))
		throw std::runtime_error("cannot commit the PNG stream");

	HGLOBAL handle = nullptr;
	if (FAILED(GetHGlobalFromStream(stream.get(), &handle)))
		throw std::runtime_error("cannot read the encoded image");

	auto size = GlobalSize(handle);
	auto * data = static_cast<const std::byte *>(GlobalLock(handle));
	if (not data)
		throw std::runtime_error("cannot lock the encoded image");

	std::vector<std::byte> result(data, data + size);
	GlobalUnlock(handle);
	return result;
}
} // namespace

namespace wivrn
{
const std::vector<icon> & load_icon(const std::filesystem::path & filename)
{
	static std::unordered_map<std::wstring, std::vector<icon>> cache;
	static com_initializer initialize_com;
	static com_ptr<IWICImagingFactory> factory;

	auto key = filename.wstring();
	if (auto it = cache.find(key); it != cache.end())
		return it->second;

	if (not factory)
	{
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
			throw std::runtime_error("cannot create the WIC imaging factory");
	}

	auto wide_filename = widen(filename);
	if (wide_filename.empty())
		throw std::runtime_error("empty icon filename");

	com_ptr<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromFilename(wide_filename.c_str(),
	                                              nullptr,
	                                              GENERIC_READ,
	                                              WICDecodeMetadataCacheOnDemand,
	                                              &decoder)))
		throw std::runtime_error("unsupported icon format: " + filename.string());

	com_ptr<IWICBitmapFrameDecode> frame_decode;
	if (FAILED(decoder->GetFrame(0, &frame_decode)))
		throw std::runtime_error("icon has no frame: " + filename.string());

	UINT width = 0;
	UINT height = 0;
	if (FAILED(frame_decode->GetSize(&width, &height)) or width == 0 or height == 0)
		throw std::runtime_error("invalid icon size: " + filename.string());

	com_ptr<IWICFormatConverter> converter;
	if (FAILED(factory->CreateFormatConverter(&converter)))
		throw std::runtime_error("cannot create the WIC format converter");
	if (FAILED(converter->Initialize(frame_decode.get(),
	                                 GUID_WICPixelFormat32bppBGRA,
	                                 WICBitmapDitherTypeNone,
	                                 nullptr,
	                                 0.0,
	                                 WICBitmapPaletteTypeCustom)))
		throw std::runtime_error("cannot convert the icon: " + filename.string());

	std::vector<icon> icons;
	icons.push_back(icon{
	        .width = (int)width,
	        .height = (int)height,
	        .bpp = 32,
	        .png_data = encode_png(factory.get(), converter.get(), width, height),
	});

	return cache.emplace(key, std::move(icons)).first->second;
}
} // namespace wivrn
