export interface EncoderProbeMetadata {
  probed?: boolean;
  h264_available?: boolean;
  hevc_available?: boolean;
  av1_available?: boolean;
  pyrowave_compiled?: boolean;
  pyrowave_advertised?: boolean;
}

export function encoderCodecLabels(probe?: EncoderProbeMetadata): string[] {
  if (!probe) return [];

  const labels: string[] = [];
  if (probe.probed) {
    if (probe.h264_available) labels.push('H.264');
    if (probe.hevc_available) labels.push('HEVC');
    if (probe.av1_available) labels.push('AV1');
  }
  return labels;
}
