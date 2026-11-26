#pragma once

// H.264 a=fmtp parsing: packetization-mode/profile-level-id lookup and
// sprop-parameter-sets -> Annex-B parameter-set AU. Pure functions: no
// logging, no platform code, no shared state.

#include <cstdint>
#include <string>
#include <vector>

namespace vcs {
	namespace media {
		namespace h264sdp {

			inline std::string TrimCopy(const std::string& s) {
				size_t b = 0, e = s.size();
				while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
				while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
				return s.substr(b, e - b);
			}

			// "profile-level-id=42e01e;packetization-mode=1" -> value, or ""
			inline std::string FmtpParam(const std::string& fmtp, const char* key) {
				size_t pos = 0;
				while (pos < fmtp.size()) {
					size_t end = fmtp.find(';', pos);
					if (end == std::string::npos) end = fmtp.size();
					const size_t eq = fmtp.find('=', pos);
					if (eq != std::string::npos && eq < end &&
						TrimCopy(fmtp.substr(pos, eq - pos)) == key) {
						return TrimCopy(fmtp.substr(eq + 1, end - eq - 1));
					}
					pos = end + 1;
				}
				return std::string();
			}

			inline bool Base64Decode(const std::string& in, std::vector<uint8_t>& out) {
				out.clear();
				int val = 0, bits = -8;
				for (unsigned char c : in) {
					if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
					int d = -1;
					if (c >= 'A' && c <= 'Z') d = c - 'A';
					else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
					else if (c >= '0' && c <= '9') d = c - '0' + 52;
					else if (c == '+') d = 62;
					else if (c == '/') d = 63;
					else if (c == '=') break;
					else return false;
					val = (val << 6) | d;
					bits += 6;
					if (bits >= 0) {
						out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
						bits -= 8;
					}
				}
				return !out.empty();
			}

			// "SPS_base64,PPS_base64" -> Annex-B AU "<00 00 00 01 SPS><.. PPS>".
			// Only NAL types 7 and 8 are accepted; anything else is treated as
			// a malformed sprop (the caller logs it as our parsing failure).
			inline bool BuildParamSetAuFromSprop(const std::string& sprop,
				std::vector<uint8_t>& au, std::string* nalTypes) {
				au.clear();
				if (nalTypes) nalTypes->clear();
				size_t pos = 0;
				while (pos <= sprop.size()) {
					const size_t comma = sprop.find(',', pos);
					const std::string item = TrimCopy(sprop.substr(pos,
						comma == std::string::npos ? std::string::npos : comma - pos));
					std::vector<uint8_t> nal;
					if (!Base64Decode(item, nal)) return false;

					size_t off = 0;
					if (nal.size() >= 4 && nal[0] == 0 && nal[1] == 0 &&
						nal[2] == 0 && nal[3] == 1) {
						off = 4;
					}
					else if (nal.size() >= 3 && nal[0] == 0 && nal[1] == 0 &&
						nal[2] == 1) {
						off = 3;
					}
					if (nal.size() <= off) return false;
					const uint8_t type = nal[off] & 0x1F;
					if (type != 7 && type != 8) return false;

					static const uint8_t kStartCode[4] = { 0, 0, 0, 1 };
					au.insert(au.end(), kStartCode, kStartCode + 4);
					au.insert(au.end(), nal.begin() + static_cast<std::ptrdiff_t>(off),
						nal.end());
					if (nalTypes) {
						if (!nalTypes->empty()) *nalTypes += ",";
						*nalTypes += std::to_string(type);
					}
					if (comma == std::string::npos) break;
					pos = comma + 1;
				}
				return !au.empty();
			}

		} // namespace h264sdp
	} // namespace media
} // namespace vcs
