// Copyright (c) 2005-2006, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "utils.h"

#include "compat.h"
#include "format.h"
#include "options.h"
#include "retina_helper.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>

#ifdef __UNIX__
#include <unistd.h>
#endif
#include <boost/filesystem/path.hpp>
#include <map>
#include <unicode/locid.h>
#include <unicode/unistr.h>
#include <wx/arrstr.h>
#include <wx/clipbrd.h>
#include <wx/filedlg.h>
#include <wx/stdpaths.h>
#include <wx/window.h>

#ifdef __APPLE__
#include <libaegisub/util_osx.h>
#include <CoreText/CTFont.h>
#endif

/// @brief There shall be no kiB, MiB stuff here Pretty reading of size
wxString PrettySize(int bytes) {
	const char *suffix[] = { "", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" };

	// Set size
	size_t i = 0;
	double size = bytes;
	while (size > 1024 && i + 1 < sizeof(suffix) / sizeof(suffix[0])) {
		size /= 1024.0;
		i++;
	}

	// Set number of decimal places
	const char *fmt = "%.0f";
	if (size < 10)
		fmt = "%.2f";
	else if (size < 100)
		fmt = "%1.f";
	return agi::wxformat(fmt, size) + " " + suffix[i];
}

namespace {
/// Decode one UTF-8 code point starting at i; advances i. Invalid bytes are
/// returned as-is so the caller never loops forever.
uint32_t NextCodePoint(std::string const& text, size_t& i) {
	const unsigned char c = static_cast<unsigned char>(text[i]);
	int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
	if (i + len > text.size())
		len = 1;
	uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
	for (int k = 1; k < len; ++k)
		cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
	i += len;
	return cp;
}

bool IsRtlCodePoint(uint32_t cp) {
	return (cp >= 0x0590 && cp <= 0x08FF)   // Hebrew, Arabic, Syriac, Thaana, N'Ko, Samaritan, Mandaic, Arabic Extended
		|| (cp >= 0xFB1D && cp <= 0xFDFF)   // Hebrew & Arabic presentation forms A
		|| (cp >= 0xFE70 && cp <= 0xFEFF);  // Arabic presentation forms B
}

bool IsBidiControl(uint32_t cp) {
	return cp == 0x200E || cp == 0x200F || cp == 0x061C
		|| (cp >= 0x202A && cp <= 0x202E)
		|| (cp >= 0x2066 && cp <= 0x2069);
}

const std::string rtl_embedding = "\xE2\x80\xAB"; // U+202B RIGHT-TO-LEFT EMBEDDING
}

bool HasRtlText(std::string const& text) {
	bool in_block = false;
	for (size_t i = 0; i < text.size();) {
		const char ch = text[i];
		if (ch == '{') in_block = true;
		else if (ch == '}') in_block = false;
		const uint32_t cp = NextCodePoint(text, i);
		if (!in_block && IsRtlCodePoint(cp))
			return true;
	}
	return false;
}

std::string AddRtlMarks(std::string const& text) {
	std::string out;
	out.reserve(text.size() + 3 * 4);
	size_t i = 0;

	auto start_visual_line = [&] {
		// Keep leading override blocks first so tags stay where tools expect them.
		while (i < text.size() && text[i] == '{') {
			const size_t close = text.find('}', i);
			if (close == std::string::npos)
				break;
			out.append(text, i, close - i + 1);
			i = close + 1;
		}
		if (text.compare(i, rtl_embedding.size(), rtl_embedding) != 0)
			out += rtl_embedding;
	};

	start_visual_line();
	bool in_block = false;
	while (i < text.size()) {
		const char ch = text[i];
		if (ch == '{')
			in_block = true;
		else if (ch == '}')
			in_block = false;
		else if (!in_block && ch == '\\' && i + 1 < text.size() && (text[i + 1] == 'N' || text[i + 1] == 'n')) {
			out.append(text, i, 2);
			i += 2;
			start_visual_line();
			continue;
		}
		out += ch;
		++i;
	}
	return out;
}

std::string RemoveBidiMarks(std::string const& text) {
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size();) {
		const size_t start = i;
		const uint32_t cp = NextCodePoint(text, i);
		if (!IsBidiControl(cp))
			out.append(text, start, i - start);
	}
	return out;
}

std::string float_to_string(double val, int precision) {
	std::string fmt = "%." + std::to_string(precision) + "f";
	std::string s = agi::format(fmt, val);
	size_t pos = s.find_last_not_of("0");
	if (pos != s.find(".")) ++pos;
	s.erase(begin(s) + pos, end(s));
	return s;
}

int SmallestPowerOf2(int x) {
	x--;
	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
	x++;
	return x;
}

#ifndef __WXMAC__
void RestartAegisub(wxArrayString const& arguments) {
	std::vector<wxString> command;
	command.reserve(arguments.size() + 1);
	command.push_back(wxStandardPaths::Get().GetExecutablePath());
	for (auto const& argument : arguments)
		command.push_back(argument);

	// wxWidgets 3.0 does not have the wxArrayString overload of wxExecute.
	// Keep the arguments separate (and paths with spaces safe) via its argv API.
	std::vector<wxChar *> argv;
	argv.reserve(command.size() + 1);
	for (auto& argument : command) {
		const wxChar *value = argument.c_str();
		argv.push_back(const_cast<wxChar *>(value));
	}
	argv.push_back(nullptr);
	wxExecute(argv.data(), wxEXEC_ASYNC);
}
#endif

bool ForwardMouseWheelEvent(wxWindow *source, wxMouseEvent &evt) {
	wxWindow *target = wxFindWindowAtPoint(wxGetMousePosition());
	if (!target || target == source) return true;

	// If the mouse is over a parent of the source window just pretend it's
	// over the source window, so that the mouse wheel works on borders and such
	wxWindow *parent = source->GetParent();
	while (parent && parent != target) parent = parent->GetParent();
	if (parent == target) return true;

	// Otherwise send it to the new target
	target->GetEventHandler()->ProcessEvent(evt);
	evt.Skip(false);
	return false;
}

std::string GetClipboard() {
	wxString data;
	wxClipboard *cb = wxClipboard::Get();
	if (cb->Open()) {
		if (cb->IsSupported(wxDF_TEXT) || cb->IsSupported(wxDF_UNICODETEXT)) {
			wxTextDataObject raw_data;
			cb->GetData(raw_data);
			data = raw_data.GetText();
		}
		cb->Close();
	}
	return from_wx(data);
}

void SetClipboard(std::string const& new_data) {
	wxClipboard *cb = wxClipboard::Get();
	if (cb->Open()) {
		cb->SetData(new wxTextDataObject(to_wx(new_data)));
		cb->Flush();
		cb->Close();
	}
}

void SetClipboard(wxBitmap const& new_data) {
	wxClipboard *cb = wxClipboard::Get();
	if (cb->Open()) {
		cb->SetData(new wxBitmapDataObject(new_data));
		cb->Flush();
		cb->Close();
	}
}

void CleanCache(agi::fs::path const& directory, std::string const& file_type, uint64_t max_size, uint64_t max_files) {
	static std::unique_ptr<agi::dispatch::Queue> queue;
	if (!queue)
		queue = agi::dispatch::Create();

	max_size <<= 20;
	if (max_files == 0)
		max_files = std::numeric_limits<uint64_t>::max();
	queue->Async([=]{
		LOG_D("utils/clean_cache") << "cleaning " << directory/file_type;
		uint64_t total_size = 0;
		using cache_item = std::pair<int64_t, agi::fs::path>;
		std::vector<cache_item> cachefiles;
		for (auto const& file : agi::fs::DirectoryIterator(directory, file_type)) {
			agi::fs::path path = directory/file;
			cachefiles.push_back({agi::fs::ModifiedTime(path), path});
			total_size += agi::fs::Size(path);
		}

		if (cachefiles.size() <= max_files && total_size <= max_size) {
			LOG_D("utils/clean_cache") << agi::format("cache does not need cleaning (maxsize=%d, cursize=%d, maxfiles=%d, numfiles=%d), exiting"
				, max_size, total_size, max_files, cachefiles.size());
			return;
		}

		sort(begin(cachefiles), end(cachefiles), [](cache_item const& a, cache_item const& b) {
			return a.first < b.first;
		});

		int deleted = 0;
		for (auto const& i : cachefiles) {
			// stop cleaning?
			if ((total_size <= max_size && cachefiles.size() - deleted <= max_files) || cachefiles.size() - deleted < 2)
				break;

			uint64_t size = agi::fs::Size(i.second);
			try {
				agi::fs::Remove(i.second);
				LOG_D("utils/clean_cache") << "deleted " << i.second;
			}
			catch  (agi::Exception const& e) {
				LOG_D("utils/clean_cache") << "failed to delete file " << i.second << ": " << e.GetMessage();
				continue;
			}

			total_size -= size;
			++deleted;
		}

		LOG_D("utils/clean_cache") << "deleted " << deleted << " files, exiting";
	});
}

#ifndef __WXOSX_COCOA__
// OS X implementation in osx_utils.mm
void AddFullScreenButton(wxWindow *) { }
void SetFloatOnParent(wxWindow *) { }

// OS X implementation in retina_helper.mm
RetinaHelper::RetinaHelper(wxWindow* w) { window = w; }
RetinaHelper::~RetinaHelper() { }
int RetinaHelper::GetScaleFactor() const {
#ifdef __WXGTK__
	return int(window->GetContentScaleFactor());
#else
	return 1;
#endif
}

// OS X implementation in scintilla_ime.mm
namespace osx { namespace ime {
	void inject(wxStyledTextCtrl *) { }
	void invalidate(wxStyledTextCtrl *) { }
	bool process_key_event(wxStyledTextCtrl *, wxKeyEvent&) { return false; }
} }
#endif

wxString FontFace(std::string opt_prefix) {
	opt_prefix += "/Font Face";
	auto value = OPT_GET(opt_prefix)->GetString();
#ifdef __WXOSX_COCOA__
	if (value.empty()) {
		auto default_font = CTFontCreateUIFontForLanguage(kCTFontUserFontType, 0, nullptr);
		auto default_font_name = CTFontCopyPostScriptName(default_font);
		CFRelease(default_font);

		auto utf8_str = CFStringGetCStringPtr(default_font_name, kCFStringEncodingUTF8);
		if (utf8_str)
			value = utf8_str;
		else {
			char buffer[1024];
			CFStringGetCString(default_font_name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
			buffer[1023] = '\0';
			value = buffer;
		}

		CFRelease(default_font_name);
	}
#endif
	return to_wx(value);
}

static agi::fs::path FileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, int flags, wxWindow *parent) {
	wxString path;
	if (!option_name.empty())
		path = to_wx(OPT_GET(option_name)->GetString());
	agi::fs::path filename = wxFileSelector(message, path, to_wx(default_filename), to_wx(default_extension), to_wx(wildcard), flags, parent).wx_str();
	if (!filename.empty() && !option_name.empty())
		OPT_SET(option_name)->SetString(filename.parent_path().string());
	return filename;
}

agi::fs::path OpenFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent) {
	return FileSelector(message, option_name, default_filename, default_extension, wildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST, parent);
}

agi::fs::path SaveFileSelector(wxString const& message, std::string const& option_name, std::string const& default_filename, std::string const& default_extension, std::string const& wildcard, wxWindow *parent) {
	return FileSelector(message, option_name, default_filename, default_extension, wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT, parent);
}

wxString LocalizedLanguageName(wxString const& lang) {
	icu::Locale iculoc(lang.c_str());
	if (!iculoc.isBogus()) {
		icu::UnicodeString ustr;
		iculoc.getDisplayName(iculoc, ustr);
#ifdef _MSC_VER
		return wxString((const wchar_t*)ustr.getBuffer());
#else
		std::string utf8;
		ustr.toUTF8String(utf8);
		return to_wx(utf8);
#endif
	}

	if (auto info = wxLocale::FindLanguageInfo(lang))
		return info->Description;
	return lang;
}
