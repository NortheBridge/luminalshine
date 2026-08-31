import {
  applyMultiopusOffer,
  getNegotiatedAudioChannels,
  isSurroundChannelCount,
} from '@web/utils/webrtc/client';

/**
 * A trimmed-down Chrome offer. Chrome never advertises multiopus itself, which
 * is exactly why applyMultiopusOffer has to add it before setLocalDescription.
 */
const OFFER = [
  'v=0',
  'o=- 1 2 IN IP4 127.0.0.1',
  's=-',
  't=0 0',
  'm=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0 8',
  'c=IN IP4 0.0.0.0',
  'a=mid:0',
  'a=rtpmap:111 opus/48000/2',
  'a=fmtp:111 minptime=10;useinbandfec=1',
  'a=rtcp-fb:111 transport-cc',
  'a=rtpmap:63 red/48000/2',
  'a=rtpmap:9 G722/8000',
  'a=rtpmap:0 PCMU/8000',
  'a=rtpmap:8 PCMA/8000',
  'm=video 9 UDP/TLS/RTP/SAVPF 96',
  'a=mid:1',
  'a=rtpmap:96 VP8/90000',
  '',
].join('\r\n');

const audioSection = (sdp: string): string[] => {
  const lines = sdp.split('\r\n');
  const start = lines.findIndex((line) => line.startsWith('m=audio'));
  const rest = lines.slice(start + 1);
  const end = rest.findIndex((line) => line.startsWith('m='));
  return [lines[start] as string, ...(end < 0 ? rest : rest.slice(0, end))];
};

describe('isSurroundChannelCount', () => {
  test('accepts only the layouts the host can capture', () => {
    expect(isSurroundChannelCount(6)).toBe(true);
    expect(isSurroundChannelCount(8)).toBe(true);
    expect(isSurroundChannelCount(2)).toBe(false);
    expect(isSurroundChannelCount(4)).toBe(false);
    expect(isSurroundChannelCount(undefined)).toBe(false);
  });
});

describe('applyMultiopusOffer', () => {
  test('leaves stereo and unset channel counts alone', () => {
    expect(applyMultiopusOffer(OFFER, 2)).toBe(OFFER);
    expect(applyMultiopusOffer(OFFER, undefined)).toBe(OFFER);
    expect(applyMultiopusOffer(OFFER, 4)).toBe(OFFER);
  });

  test('adds a 5.1 multiopus payload and prefers it', () => {
    const munged = applyMultiopusOffer(OFFER, 6);
    const section = audioSection(munged);

    const rtpmap = section.find((line) => line.includes('multiopus/48000/6'));
    expect(rtpmap).toBeDefined();

    const payloadType = /^a=rtpmap:(\d+) /.exec(rtpmap as string)?.[1];
    expect(payloadType).toBeDefined();

    // Listed first => this is what we want negotiated.
    expect(section[0]).toBe(`m=audio 9 UDP/TLS/RTP/SAVPF ${payloadType} 111 63 9 0 8`);

    const fmtp = section.find((line) => line.startsWith(`a=fmtp:${payloadType} `)) as string;
    expect(fmtp).toContain('num_streams=6');
    expect(fmtp).toContain('coupled_streams=0');
    expect(fmtp).toContain('channel_mapping=0,1,2,3,4,5');
  });

  test('adds a 7.1 multiopus payload', () => {
    const munged = applyMultiopusOffer(OFFER, 8);
    const section = audioSection(munged);
    const rtpmap = section.find((line) => line.includes('multiopus/48000/8')) as string;
    const payloadType = /^a=rtpmap:(\d+) /.exec(rtpmap)?.[1];

    const fmtp = section.find((line) => line.startsWith(`a=fmtp:${payloadType} `)) as string;
    expect(fmtp).toContain('num_streams=8');
    expect(fmtp).toContain('channel_mapping=0,1,2,3,4,5,6,7');
  });

  test('picks a payload type that is not already taken', () => {
    const munged = applyMultiopusOffer(OFFER, 6);
    const rtpmap = audioSection(munged).find((line) => line.includes('multiopus')) as string;
    const payloadType = Number(/^a=rtpmap:(\d+) /.exec(rtpmap)?.[1]);

    expect(payloadType).toBeGreaterThanOrEqual(96);
    expect(payloadType).toBeLessThanOrEqual(127);
    expect([111, 63, 9, 0, 8, 96]).not.toContain(payloadType);
  });

  test('does not disturb the video section', () => {
    const munged = applyMultiopusOffer(OFFER, 6);
    expect(munged).toContain('m=video 9 UDP/TLS/RTP/SAVPF 96');
    expect(munged).toContain('a=rtpmap:96 VP8/90000');
    expect(munged.split('\r\n').filter((l) => l.startsWith('m=')).length).toBe(2);
  });

  test('is idempotent', () => {
    const once = applyMultiopusOffer(OFFER, 6);
    expect(applyMultiopusOffer(once, 6)).toBe(once);
  });

  test('returns the input when there is no audio section', () => {
    const videoOnly = ['v=0', 'm=video 9 UDP/TLS/RTP/SAVPF 96', 'a=rtpmap:96 VP8/90000', ''].join(
      '\r\n',
    );
    expect(applyMultiopusOffer(videoOnly, 6)).toBe(videoOnly);
  });
});

describe('getNegotiatedAudioChannels', () => {
  const answerWith = (payloads: string, rtpmaps: string[]) =>
    ['v=0', `m=audio 9 UDP/TLS/RTP/SAVPF ${payloads}`, 'a=mid:0', ...rtpmaps, ''].join('\r\n');

  test('reports surround when the host selected multiopus', () => {
    const answer = answerWith('100 111', [
      'a=rtpmap:100 multiopus/48000/6',
      'a=rtpmap:111 opus/48000/2',
    ]);
    expect(getNegotiatedAudioChannels(answer)).toBe(6);
  });

  test('reports stereo when the host fell back', () => {
    const answer = answerWith('111 100', [
      'a=rtpmap:111 opus/48000/2',
      'a=rtpmap:100 multiopus/48000/6',
    ]);
    expect(getNegotiatedAudioChannels(answer)).toBe(2);
  });

  test('treats a codec with no channel field as mono', () => {
    expect(getNegotiatedAudioChannels(answerWith('0', ['a=rtpmap:0 PCMU/8000']))).toBe(1);
  });

  test('returns undefined when it cannot tell', () => {
    expect(getNegotiatedAudioChannels('')).toBeUndefined();
    expect(getNegotiatedAudioChannels('v=0\r\n')).toBeUndefined();
    expect(getNegotiatedAudioChannels(answerWith('111', []))).toBeUndefined();
  });
});
