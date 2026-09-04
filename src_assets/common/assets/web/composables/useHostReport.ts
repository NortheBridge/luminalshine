import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import { useI18n } from 'vue-i18n';
import { useMessage } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { useT2 } from '@/composables/useT2';

/**
 * The host identity report ("About this host"): the same facts the About
 * page shows, as inspector rows and as a copyable plain-text / Markdown
 * block for bug reports. Lifted from the old AboutView so both can share it.
 */

interface GpuRow {
  description: string;
  vendor: 'nvidia' | 'amd' | 'intel' | 'other';
  vram_bytes: number;
  driver_version: string;
  driver_date: string;
  api_label: string;
  is_active: boolean;
}
interface EncoderRow {
  name: string;
  available: boolean | null;
  yuv444: boolean;
}
interface DisplayRow {
  display_name: string;
  friendly_name: string;
  hdr_supported: boolean;
  hdr_enabled: boolean;
}

function vendorOf(vendor_id?: number | string): GpuRow['vendor'] {
  const v = typeof vendor_id === 'string' ? Number(vendor_id) : vendor_id;
  if (v === 0x10de) return 'nvidia';
  if (v === 0x1002 || v === 0x1022) return 'amd';
  if (v === 0x8086) return 'intel';
  return 'other';
}
function vendorApiLabel(vendor: GpuRow['vendor']): string {
  // Reflect what LuminalShine actually links against, not aspirational SDKs.
  return vendor === 'nvidia' ? 'NVAPI · Direct3D 11 / DXGI' : 'Direct3D 11 / DXGI';
}
export function formatVram(bytes: number): string {
  if (!bytes || !Number.isFinite(bytes)) return '';
  const gib = bytes / 1024 ** 3;
  return gib >= 1 ? `${gib.toFixed(gib >= 10 ? 0 : 1)} GB` : `${Math.round(bytes / 1024 ** 2)} MB`;
}

export function useHostReport() {
  const store = useConfigStore();
  const { metadata, config } = storeToRefs(store);
  const { t } = useI18n();
  const t2 = useT2();
  const message = useMessage();

  const isWindows = computed(
    () => String(metadata.value?.platform || '').toLowerCase() === 'windows',
  );
  const version = computed(() => metadata.value?.version || '');
  const commitShort = computed(() => {
    const c = metadata.value?.commit || '';
    return c.length > 7 ? c.substring(0, 7) : c;
  });
  const branch = computed(() => metadata.value?.branch || '');
  const releaseDate = computed(() => metadata.value?.release_date || '');
  const prerelease = computed(() => metadata.value?.prerelease || '');

  const osIsInsider = computed(() => !!metadata.value?.windows_insider?.is_insider);
  const osChannelLabel = computed(() => {
    const ins = metadata.value?.windows_insider;
    if (ins?.is_insider && ins.branch_name) return ins.branch_name;
    return t('about.channel_release');
  });
  const osVersionLabel = computed(() => {
    const md = metadata.value;
    if (!md) return '';
    if (osIsInsider.value && md.windows_build_number)
      return `${t('about.build_prefix')} ${md.windows_build_number}`;
    const dv = md.windows_display_version;
    const bn = md.windows_build_number;
    if (dv && bn) return `${dv} (${t('about.build_prefix')} ${bn})`;
    if (dv) return dv;
    if (bn) return `${t('about.build_prefix')} ${bn}`;
    return '';
  });
  const osProductName = computed(() => metadata.value?.windows_product_name || '');

  // GPUs — metadata.gpus[] (DXGI) joined to metadata.gpu_drivers[] (registry)
  // on vendor_id + device_id. The first entry is the capture/encode adapter.
  const gpus = computed<GpuRow[]>(() => {
    const md = metadata.value;
    if (!md?.gpus) return [];
    const drivers = md.gpu_drivers || [];
    return md.gpus.map((g, idx) => {
      const v = typeof g.vendor_id === 'string' ? Number(g.vendor_id) : g.vendor_id;
      const d = typeof g.device_id === 'string' ? Number(g.device_id) : g.device_id;
      const drv = drivers.find((x) => x.vendor_id === v && x.device_id === d);
      const vramRaw = g.dedicated_video_memory;
      const vram = typeof vramRaw === 'string' ? Number(vramRaw) : vramRaw;
      const vendor = vendorOf(g.vendor_id);
      return {
        description: g.description || drv?.description || '',
        vendor,
        vram_bytes: typeof vram === 'number' && Number.isFinite(vram) ? vram : 0,
        driver_version: drv?.driver_version || '',
        driver_date: drv?.driver_date || '',
        api_label: vendorApiLabel(vendor),
        is_active: idx === 0,
      };
    });
  });

  const encoders = computed<EncoderRow[]>(() => {
    const p = metadata.value?.encoder_probe;
    const probed = p?.probed === true;
    const row = (name: string, avail?: boolean, yuv?: boolean): EncoderRow => ({
      name,
      available: probed ? avail === true : null,
      yuv444: yuv === true,
    });
    return [
      row('H.264', p?.h264_available, p?.h264_yuv444),
      row('HEVC', p?.hevc_available, p?.hevc_yuv444),
      row('AV1', p?.av1_available, p?.av1_yuv444),
    ];
  });
  const refFramesInvalidation = computed(
    () => metadata.value?.encoder_probe?.ref_frames_invalidation === true,
  );

  const displays = computed<DisplayRow[]>(() =>
    (metadata.value?.displays || []).map((d) => ({
      display_name: d.display_name || '',
      friendly_name: d.friendly_name || '',
      hdr_supported: d.advanced_color_supported === true,
      hdr_enabled: d.advanced_color_enabled === true,
    })),
  );

  const virtualDisplayBackend = computed(() => {
    const b = String(metadata.value?.virtual_display_backend || '');
    return b === 'luminalvgd' ? 'LuminalVGD' : b || t('about.value_none');
  });
  const virtualDisplayVersion = computed(
    () => metadata.value?.virtual_display_backend_version || '',
  );
  const virtualDisplayStatusLabel = computed(() => {
    const code = metadata.value?.virtual_display_driver_status;
    const s = code != null ? String(code) : '';
    switch (s) {
      case '0':
        return t('about.vdd_status_ready');
      case '1':
        return t('about.vdd_status_unknown');
      case '-1':
        return t('about.vdd_status_failed');
      case '-2':
        return t('about.vdd_status_version_incompatible');
      case '-3':
        return t('about.vdd_status_watchdog_failed');
      default:
        return s || t('about.value_unknown');
    }
  });

  const hostname = computed(
    () =>
      String((config.value as Record<string, unknown>)?.['sunshine_name'] || '') ||
      metadata.value?.host_name ||
      '',
  );
  const port = computed(() => {
    const p = Number((config.value as Record<string, unknown>)?.['port']);
    return Number.isFinite(p) && p > 0 ? p : null;
  });
  const sessionCount = computed(() => metadata.value?.active_session_count ?? 0);
  const realtimeHags = computed(() => {
    const v = String(
      (config.value as Record<string, unknown>)?.['nvenc_realtime_hags'] ?? '',
    ).toLowerCase();
    if (v === 'enabled' || v === 'true' || v === '1') return t('about.value_enabled');
    if (v === 'disabled' || v === 'false' || v === '0') return t('about.value_disabled');
    return t('about.value_unknown');
  });

  /** Compact key/value rows for the inspector. */
  const rows = computed<Array<[string, string]>>(() => {
    const out: Array<[string, string]> = [];
    const push = (k: string, v: string) => {
      if (v) out.push([k, v]);
    };
    push(
      'LuminalShine',
      [version.value, prerelease.value, commitShort.value].filter(Boolean).join(' · '),
    );
    if (isWindows.value)
      push(
        t2('report.os', 'OS'),
        [
          osProductName.value,
          osChannelLabel.value + (osIsInsider.value ? ' Insider' : ''),
          osVersionLabel.value,
        ]
          .filter(Boolean)
          .join(' · '),
      );
    gpus.value.forEach((g, i) =>
      push(
        i === 0 ? 'GPU' : `GPU ${i + 1}`,
        [g.description, g.driver_version, formatVram(g.vram_bytes)].filter(Boolean).join(' · '),
      ),
    );
    if (isWindows.value) {
      const enc = encoders.value
        .map(
          (e) =>
            `${e.name} ${e.available === null ? t2('report.probing', 'probing') : e.available ? t2('report.ok', 'ok') : t2('report.na', 'n/a')}`,
        )
        .join(' · ');
      push(t2('report.encoders', 'Encoders'), enc);
      push(
        t2('report.displays', 'Displays'),
        displays.value
          .map(
            (d) =>
              (d.friendly_name && d.friendly_name !== d.display_name
                ? d.friendly_name
                : d.display_name) + (d.hdr_enabled ? ' (HDR)' : ''),
          )
          .join(' · '),
      );
      push(
        t2('report.virtual_display', 'Virtual display'),
        [virtualDisplayBackend.value, virtualDisplayVersion.value, virtualDisplayStatusLabel.value]
          .filter(Boolean)
          .join(' · '),
      );
    }
    push(
      t2('report.runtime', 'Runtime'),
      [
        hostname.value,
        port.value ? `${t2('shell.port', 'Port')} ${port.value}` : '',
        `${sessionCount.value} ${t2('report.sessions', 'active')}`,
        isWindows.value ? `HAGS ${realtimeHags.value}` : '',
      ]
        .filter(Boolean)
        .join(' · '),
    );
    return out;
  });

  function buildText(format: 'plain' | 'markdown'): string {
    const lines: string[] = [];
    const head = format === 'markdown' ? '## ' : '';
    const sub = format === 'markdown' ? '### ' : '';
    const fence = format === 'markdown' ? '```' : '';
    lines.push(`${head}LuminalShine Diagnostics`, '');
    if (format === 'markdown') lines.push(fence);
    lines.push(`Generated: ${new Date().toISOString()}`, '');
    lines.push(`${sub}LuminalShine`);
    lines.push(`Version:        ${version.value}`);
    if (commitShort.value) lines.push(`Commit:         ${commitShort.value}`);
    if (branch.value) lines.push(`Branch:         ${branch.value}`);
    if (releaseDate.value) lines.push(`Release:        ${releaseDate.value}`);
    if (prerelease.value) lines.push(`Prerelease:     ${prerelease.value}`);
    lines.push('');
    if (isWindows.value) {
      lines.push(`${sub}Operating System`);
      if (osProductName.value) lines.push(`Edition:        ${osProductName.value}`);
      lines.push(`Channel:        ${osChannelLabel.value}${osIsInsider.value ? ' (Insider)' : ''}`);
      lines.push(`Version:        ${osVersionLabel.value}`, '');
    }
    lines.push(`${sub}Graphics`);
    for (const g of gpus.value) {
      lines.push(`  - ${g.description}${g.is_active ? '  [active]' : ''}`);
      if (g.vram_bytes) lines.push(`    VRAM:    ${formatVram(g.vram_bytes)}`);
      if (g.driver_version)
        lines.push(
          `    Driver:  ${g.driver_version}${g.driver_date ? ` (released ${g.driver_date})` : ''}`,
        );
      lines.push(`    API:     ${g.api_label}`);
    }
    lines.push('');
    if (isWindows.value) {
      lines.push(`${sub}Encoders`);
      for (const e of encoders.value) {
        const state =
          e.available === null
            ? 'Probing'
            : e.available
              ? `Available${e.yuv444 ? ' (YUV444)' : ''}`
              : 'Unavailable';
        lines.push(`  ${e.name.padEnd(6)} ${state}`);
      }
      if (refFramesInvalidation.value) lines.push('  Reference frame invalidation: yes');
      lines.push('');
      if (displays.value.length) {
        lines.push(`${sub}Display & HDR`);
        for (const d of displays.value) {
          const name =
            d.friendly_name && d.friendly_name !== d.display_name
              ? `${d.display_name} (${d.friendly_name})`
              : d.display_name;
          lines.push(
            `  ${name}: HDR ${d.hdr_supported ? 'supported' : 'not supported'}, ${d.hdr_enabled ? 'enabled' : 'off'}`,
          );
        }
        lines.push('');
      }
      lines.push(`${sub}Virtual Display`);
      lines.push(`Backend:        ${virtualDisplayBackend.value}`);
      if (virtualDisplayVersion.value) lines.push(`Version:        ${virtualDisplayVersion.value}`);
      lines.push(`Status:         ${virtualDisplayStatusLabel.value}`, '');
    }
    lines.push(`${sub}Runtime`);
    if (hostname.value) lines.push(`Hostname:       ${hostname.value}`);
    if (port.value !== null) lines.push(`Port:           ${port.value}`);
    lines.push(`Active streams: ${sessionCount.value}`);
    if (isWindows.value) lines.push(`HAGS hint:      ${realtimeHags.value}`);
    if (format === 'markdown') lines.push(fence);
    return lines.join('\n');
  }

  async function copy(format: 'plain' | 'markdown'): Promise<void> {
    const text = buildText(format);
    try {
      await navigator.clipboard.writeText(text);
      message.success(
        format === 'markdown' ? t('about.copied_markdown') : t('about.copied_diagnostics'),
      );
    } catch {
      message.error(t('about.copy_failed'));
    }
  }

  return { rows, buildText, copy, version, commitShort, gpus, encoders, displays, isWindows };
}
