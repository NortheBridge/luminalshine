import { computed, ref } from 'vue';
import { useMessage } from 'naive-ui';
import { http } from '@/http';
import { useHostStore } from '@/stores/host';
import { useT2 } from '@/composables/useT2';

/**
 * Crash-dump actions for the Overview: export the (possibly multi-part)
 * crash bundle and dismiss the notification. Status itself lives in the
 * host store.
 */
export function useCrashDump() {
  const host = useHostStore();
  const message = useMessage();
  const t2 = useT2();
  const exportPending = ref(false);

  function parseContentDispositionFilename(header?: string): string | null {
    if (!header) return null;
    const filenameStar = /filename\*=UTF-8''([^;]+)/i.exec(header);
    if (filenameStar?.[1]) {
      try {
        return decodeURIComponent(filenameStar[1]);
      } catch {
        return filenameStar[1];
      }
    }
    const filenameMatch = /filename="?([^";]+)"?/i.exec(header);
    return filenameMatch?.[1] || null;
  }

  function triggerDownload(blob: Blob, filename: string) {
    const url = window.URL.createObjectURL(blob);
    const link = window.document.createElement('a');
    link.href = url;
    link.download = filename;
    link.click();
    window.URL.revokeObjectURL(url);
  }

  async function downloadPart(partIndex: number, filenameHint?: string) {
    const r = await http.get(`/api/logs/export_crash?part=${partIndex}`, {
      responseType: 'blob',
      validateStatus: () => true,
    });
    if (r.status !== 200) {
      throw new Error('crash bundle download failed');
    }
    const rawHeader: unknown = r.headers?.['content-disposition'];
    const headerName = parseContentDispositionFilename(
      typeof rawHeader === 'string' ? rawHeader : undefined,
    );
    const filename = filenameHint || headerName || `sunshine_crashbundle-part${partIndex}.zip`;
    triggerDownload(r.data as Blob, filename);
  }

  async function exportBundle(): Promise<void> {
    if (exportPending.value || typeof window === 'undefined') return;
    exportPending.value = true;
    try {
      const manifest = await http.get('/api/logs/export_crash/manifest', {
        validateStatus: () => true,
      });
      const manifestBody = manifest.data as { parts?: unknown } | undefined;
      const parts = (Array.isArray(manifestBody?.parts) ? manifestBody.parts : []) as Array<{
        index?: number | string;
        filename?: string;
      }>;
      if (manifest.status === 200 && parts.length > 0) {
        const ordered = [...parts].sort((a, b) => Number(a.index) - Number(b.index));
        for (const part of ordered) {
          const index = Number(part.index) || 0;
          if (index <= 0) continue;
          await downloadPart(index, part.filename);
        }
      } else {
        await downloadPart(1);
      }
    } catch {
      message.error(t2('config.crash_dump_export_error', 'Failed to export crash bundle.'));
    } finally {
      exportPending.value = false;
    }
  }

  async function dismiss(): Promise<void> {
    const current = host.crashDump;
    if (!current?.available) return;
    const payload = { filename: current.filename, captured_at: current.captured_at };
    if (!payload.filename || !payload.captured_at) {
      message.error(t2('config.crash_dump_dismiss_error', 'Failed to dismiss crash notification.'));
      return;
    }
    try {
      const r = await http.post('/api/health/crashdump/dismiss', payload, {
        validateStatus: () => true,
      });
      const body = r.data as
        | { status?: boolean; dismissed_at?: string; error?: string; message?: string }
        | undefined;
      if (r.status === 200 && body?.status === true) {
        host.setCrashDump({
          ...current,
          dismissed: true,
          dismissed_at: body.dismissed_at || new Date().toISOString(),
        });
        message.success(t2('config.crash_dump_dismiss_success', 'Crash notification dismissed.'));
        await host.refreshCrashDump();
      } else {
        const errData = body && (body.error || body.message);
        const errMessage = typeof errData === 'string' ? errData : '';
        if (errMessage) {
          const lower = errMessage.toLowerCase();
          if (
            lower.includes('metadata mismatch') ||
            lower.includes('no recent sunshine crash dumps')
          ) {
            await host.refreshCrashDump();
          }
        }
        message.error(
          errMessage ||
            t2('config.crash_dump_dismiss_error', 'Failed to dismiss crash notification.'),
        );
      }
    } catch {
      await host.refreshCrashDump();
      message.error(t2('config.crash_dump_dismiss_error', 'Failed to dismiss crash notification.'));
    }
  }

  function humanFileSize(bytes?: number | null) {
    if (typeof bytes !== 'number' || !Number.isFinite(bytes) || bytes <= 0) return '';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit += 1;
    }
    const formatter = new Intl.NumberFormat(undefined, {
      maximumFractionDigits: value >= 10 ? 0 : 1,
    });
    return `${formatter.format(value)} ${units[unit] ?? 'B'}`;
  }

  const details = computed(() => {
    const d = host.crashDump;
    if (!d || !d.available) return '';
    const parts: string[] = [];
    if (d.filename) parts.push(d.filename);
    if (typeof d.size_bytes === 'number') {
      const size = humanFileSize(d.size_bytes);
      if (size) parts.push(size);
    }
    if (d.captured_at) {
      const captured = new Date(d.captured_at);
      if (!Number.isNaN(captured.getTime())) parts.push(captured.toLocaleString());
    }
    return parts.join(' · ');
  });

  return { exportPending, exportBundle, dismiss, details };
}
