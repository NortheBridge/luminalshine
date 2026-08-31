/**
 * @file tests/unit/test_webrtc_sdp.cpp
 * @brief Unit tests for the WebRTC answer-SDP audio rewrite.
 */

#include "../tests_common.h"

#include <src/audio.h>
#include <src/webrtc_stream.h>

namespace {
  /**
   * @brief Build an answer SDP whose audio section carries the given payload lines.
   *
   * Uses CRLF throughout, like a real browser, so the tests also pin down that
   * the rewrite preserves line endings.
   */
  std::string make_answer(const std::string &payload_types, const std::string &codec_lines) {
    return "v=0\r\n"
           "o=- 1 2 IN IP4 127.0.0.1\r\n"
           "s=-\r\n"
           "t=0 0\r\n"
           "m=audio 9 UDP/TLS/RTP/SAVPF " +
           payload_types +
           "\r\n"
           "c=IN IP4 0.0.0.0\r\n"
           "a=mid:0\r\n" +
           codec_lines +
           "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
           "a=mid:1\r\n"
           "a=rtpmap:96 H264/90000\r\n"
           "a=fmtp:96 profile-level-id=42e01f\r\n";
  }

  const std::string kStereoOnly = make_answer(
    "111 63",
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=rtpmap:63 red/48000/2\r\n"
  );

  /// Chrome only ever offers multiopus when the page munges its own offer.
  const std::string kWithMultiopus51 = make_answer(
    "111 100 63",
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=rtpmap:100 multiopus/48000/6\r\n"
    "a=fmtp:100 minptime=10;useinbandfec=1;num_streams=4;coupled_streams=2;channel_mapping=0,4,1,2,3,5\r\n"
    "a=rtpmap:63 red/48000/2\r\n"
  );

  const std::string kWithMultiopus71 = make_answer(
    "111 100",
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=rtpmap:100 multiopus/48000/8\r\n"
    "a=fmtp:100 minptime=10;useinbandfec=1;num_streams=8;coupled_streams=0;channel_mapping=0,1,2,3,4,5,6,7\r\n"
  );

  /// Extract the parameters of `a=fmtp:<pt> ...`, or "" when absent.
  std::string fmtp_for(const std::string &sdp, const std::string &payload_type) {
    const std::string needle = "a=fmtp:" + payload_type + " ";
    auto at = sdp.find(needle);
    if (at == std::string::npos) {
      return {};
    }
    at += needle.size();
    return sdp.substr(at, sdp.find("\r\n", at) - at);
  }

  /// Extract the `m=audio` line, or "" when absent.
  std::string audio_m_line(const std::string &sdp) {
    auto at = sdp.find("m=audio ");
    if (at == std::string::npos) {
      return {};
    }
    return sdp.substr(at, sdp.find("\r\n", at) - at);
  }

  bool contains(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
  }
}  // namespace

TEST(WebRtcOpusSdpTest, StereoGetsStereoParams) {
  const auto result = webrtc_stream::apply_opus_audio_params(kStereoOnly, 2);

  EXPECT_EQ(result.channels, 2);
  EXPECT_FALSE(result.multiopus);

  const auto fmtp = fmtp_for(result.sdp, "111");
  EXPECT_TRUE(contains(fmtp, "stereo=1"));
  EXPECT_TRUE(contains(fmtp, "sprop-stereo=1"));
  EXPECT_TRUE(contains(fmtp, "cbr=1"));
  EXPECT_TRUE(contains(fmtp, "usedtx=0"));
  EXPECT_TRUE(contains(fmtp, "maxaveragebitrate=" + std::to_string(audio::stream_configs[audio::HIGH_STEREO].bitrate)));
}

TEST(WebRtcOpusSdpTest, PreservesParamsWeDoNotOwn) {
  const auto result = webrtc_stream::apply_opus_audio_params(kStereoOnly, 2);
  const auto fmtp = fmtp_for(result.sdp, "111");

  EXPECT_TRUE(contains(fmtp, "minptime=10"));
  EXPECT_TRUE(contains(fmtp, "useinbandfec=1"));
}

TEST(WebRtcOpusSdpTest, PreservesCrlfAndUnrelatedSections) {
  const auto result = webrtc_stream::apply_opus_audio_params(kStereoOnly, 2);

  EXPECT_EQ(result.sdp.find('\n'), result.sdp.find("\r\n") + 1) << "line endings must stay CRLF";
  EXPECT_TRUE(contains(result.sdp, "a=fmtp:96 profile-level-id=42e01f")) << "video fmtp must be untouched";
  EXPECT_TRUE(contains(result.sdp, "m=video 9 UDP/TLS/RTP/SAVPF 96"));
}

TEST(WebRtcOpusSdpTest, SurroundWithoutMultiopusOfferFallsBackToStereo) {
  // The peer asked for 5.1 but only offered plain Opus. An answer may not
  // introduce a codec the offer left out, so stereo is the only honest result.
  const auto result = webrtc_stream::apply_opus_audio_params(kStereoOnly, 6);

  EXPECT_EQ(result.channels, 2);
  EXPECT_FALSE(result.multiopus);

  const auto fmtp = fmtp_for(result.sdp, "111");
  EXPECT_TRUE(contains(fmtp, "stereo=1"));
  EXPECT_FALSE(contains(fmtp, "channel_mapping"));
  EXPECT_EQ(audio_m_line(result.sdp), "m=audio 9 UDP/TLS/RTP/SAVPF 111 63");
}

TEST(WebRtcOpusSdpTest, SurroundSelectsMultiopusAndRestatesOurLayout) {
  const auto result = webrtc_stream::apply_opus_audio_params(kWithMultiopus51, 6);

  EXPECT_EQ(result.channels, 6);
  EXPECT_TRUE(result.multiopus);

  // The offer proposed 4 streams / 2 coupled with a rotated mapping; we answer
  // with the layout audio::capture() actually produces.
  const auto &expected = audio::stream_configs[audio::HIGH_SURROUND51];
  const auto fmtp = fmtp_for(result.sdp, "100");
  EXPECT_TRUE(contains(fmtp, "num_streams=" + std::to_string(expected.streams)));
  EXPECT_TRUE(contains(fmtp, "coupled_streams=" + std::to_string(expected.coupledStreams)));
  EXPECT_TRUE(contains(fmtp, "channel_mapping=0,1,2,3,4,5"));
  EXPECT_TRUE(contains(fmtp, "maxaveragebitrate=" + std::to_string(expected.bitrate)));
  EXPECT_FALSE(contains(fmtp, "stereo=1")) << "stereo= is meaningless for multiopus";
  EXPECT_TRUE(contains(fmtp, "minptime=10")) << "unowned params still survive";

  // Selected codec must lead the payload list.
  EXPECT_EQ(audio_m_line(result.sdp), "m=audio 9 UDP/TLS/RTP/SAVPF 100 111 63");
}

TEST(WebRtcOpusSdpTest, SevenOneUsesEightChannelMapping) {
  const auto result = webrtc_stream::apply_opus_audio_params(kWithMultiopus71, 8);

  EXPECT_EQ(result.channels, 8);
  EXPECT_TRUE(result.multiopus);

  const auto &expected = audio::stream_configs[audio::HIGH_SURROUND71];
  const auto fmtp = fmtp_for(result.sdp, "100");
  EXPECT_TRUE(contains(fmtp, "channel_mapping=0,1,2,3,4,5,6,7"));
  EXPECT_TRUE(contains(fmtp, "num_streams=" + std::to_string(expected.streams)));
  EXPECT_TRUE(contains(fmtp, "maxaveragebitrate=" + std::to_string(expected.bitrate)));
}

TEST(WebRtcOpusSdpTest, MismatchedMultiopusChannelCountIsIgnored) {
  // A 5.1 payload cannot serve a 7.1 request.
  const auto result = webrtc_stream::apply_opus_audio_params(kWithMultiopus51, 8);

  EXPECT_EQ(result.channels, 2);
  EXPECT_FALSE(result.multiopus);
  EXPECT_TRUE(contains(fmtp_for(result.sdp, "111"), "stereo=1"));
}

TEST(WebRtcOpusSdpTest, UnsupportedChannelCountFallsBackToStereo) {
  // map_stream() only knows 2/6/8; anything else must not be treated as valid.
  const auto result = webrtc_stream::apply_opus_audio_params(kWithMultiopus51, 4);

  EXPECT_EQ(result.channels, 2);
  EXPECT_FALSE(result.multiopus);
}

TEST(WebRtcOpusSdpTest, SdpWithoutOpusIsLeftAlone) {
  const auto sdp = make_answer("8", "a=rtpmap:8 PCMA/8000\r\n");
  const auto result = webrtc_stream::apply_opus_audio_params(sdp, 6);

  EXPECT_EQ(result.sdp, sdp);
  EXPECT_EQ(result.channels, 2);
  EXPECT_FALSE(result.multiopus);
}

TEST(WebRtcOpusSdpTest, InsertsFmtpWhenTheOfferOmittedIt) {
  const auto sdp = make_answer("111", "a=rtpmap:111 opus/48000/2\r\n");
  const auto result = webrtc_stream::apply_opus_audio_params(sdp, 2);

  const auto fmtp = fmtp_for(result.sdp, "111");
  EXPECT_TRUE(contains(fmtp, "stereo=1"));
  EXPECT_TRUE(contains(fmtp, "cbr=1"));
  // The synthesized line must land inside the audio section.
  EXPECT_LT(result.sdp.find("a=fmtp:111"), result.sdp.find("m=video"));
}
