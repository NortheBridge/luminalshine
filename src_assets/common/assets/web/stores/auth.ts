import { defineStore } from 'pinia';
import { ref, Ref } from 'vue';
import { http, refreshSession } from '@/http';

const rememberStorageKey = 'sunshine.auth.remember';

function readRememberPreference(): boolean {
  if (typeof window === 'undefined') return false;
  try {
    return window.localStorage.getItem(rememberStorageKey) === '1';
  } catch {
    return false;
  }
}

interface AuthStatusResponse {
  credentials_configured?: boolean;
  credentials_locked?: boolean;
  authenticated?: boolean;
  login_required?: boolean;
  role?: string;
}

export interface AuthSession {
  id: string;
  username: string;
  created_at: number;
  expires_at: number;
  refresh_expires_at?: number;
  last_seen: number;
  remember_me: boolean;
  current: boolean;
  user_agent?: string;
  remote_address?: string;
  device_label?: string;
}

type AuthListener = () => void;
interface RequireLoginOptions {
  bypassLogoutGuard?: boolean;
}

// Auth store now driven by axios/http interceptor layer instead of polling.
// Provides subscription for login events and a setter used by http.js.
export const useAuthStore = defineStore('auth', () => {
  const isAuthenticated: Ref<boolean> = ref(false);
  const ready: Ref<boolean> = ref(false);
  const _listeners: AuthListener[] = [];
  const showLoginModal: Ref<boolean> = ref(false);
  const credentialsConfigured: Ref<boolean> = ref(true);
  // Credentials exist on the host but are temporarily unloadable (e.g.
  // TPM not ready at service start). Setup must not be offered.
  const credentialsLocked: Ref<boolean> = ref(false);
  // Session privilege from /api/auth/status: 'admin' (default) or 'stats'.
  const role: Ref<string> = ref('admin');
  // Set when the backend rejects API calls with error === 'origin_forbidden';
  // carries the host's allowed origin class ('pc' | 'lan' | 'wan').
  const originForbiddenAllowed: Ref<string> = ref('');
  const loggingIn: Ref<boolean> = ref(false);
  const logoutInitiated: Ref<boolean> = ref(false);
  const sessions: Ref<AuthSession[]> = ref([]);
  const sessionsLoading: Ref<boolean> = ref(false);
  const sessionsError: Ref<string> = ref('');

  function setAuthenticated(v: boolean): void {
    if (v === isAuthenticated.value) return;
    const becameAuthed = !isAuthenticated.value && v;
    isAuthenticated.value = v;
    if (becameAuthed) {
      logoutInitiated.value = false;
      fetchSessions().catch(() => {});
      for (const cb of _listeners) {
        try {
          cb();
        } catch (e) {
          console.error('auth listener error', e);
        }
      }
    }
    if (!v) {
      sessions.value = [];
      sessionsError.value = '';
    }
  }

  function initiateLogout(): void {
    // Mark that this logout was user-initiated so we suppress login prompts
    logoutInitiated.value = true;
    // Immediately reflect unauthenticated state and hide login modal
    setAuthenticated(false);
    showLoginModal.value = false;
  }

  // Single init call invoked during app bootstrap after http layer validation.
  async function init(): Promise<void> {
    if (ready.value) return;
    const preferRemember = readRememberPreference();

    const fetchStatus = async (): Promise<AuthStatusResponse | null> => {
      try {
        const res = await http.get<AuthStatusResponse>('/api/auth/status', {
          validateStatus: () => true,
        });
        if (res && res.status === 200 && res.data) {
          return res.data;
        }
      } catch {
        /* noop */
      }
      return null;
    };

    const applyStatus = (payload: AuthStatusResponse | null): boolean => {
      if (!payload) return false;
      if (typeof payload.credentials_configured === 'boolean') {
        credentialsConfigured.value = payload.credentials_configured;
      }
      if (typeof payload.credentials_locked === 'boolean') {
        credentialsLocked.value = payload.credentials_locked;
      }
      if (payload.authenticated) {
        role.value = payload.role === 'stats' ? 'stats' : 'admin';
        setAuthenticated(true);
      }
      return !!(payload.login_required && !payload.authenticated);
    };

    try {
      let status = await fetchStatus();
      let requiresLogin = applyStatus(status);

      if (requiresLogin && !logoutInitiated.value) {
        const refreshed = await refreshSession();
        if (refreshed) {
          status = await fetchStatus();
          requiresLogin = applyStatus(status);
        }
      }

      if (requiresLogin && preferRemember && !logoutInitiated.value) {
        const retryDelays = [250, 600];
        for (const delay of retryDelays) {
          await new Promise<void>((resolve) => setTimeout(resolve, delay));
          status = await fetchStatus();
          requiresLogin = applyStatus(status);
          if (!requiresLogin) {
            break;
          }
        }
      }

      if (requiresLogin && !logoutInitiated.value) {
        showLoginModal.value = true;
      }
    } finally {
      ready.value = true;
    }
  }

  function onLogin(cb: AuthListener): () => void {
    if (typeof cb !== 'function') return () => {};
    _listeners.push(cb);
    if (isAuthenticated.value)
      setTimeout(() => {
        try {
          cb();
        } catch {}
      }, 0);
    return () => {
      const idx = _listeners.indexOf(cb);
      if (idx !== -1) _listeners.splice(idx, 1);
    };
  }

  function requireLogin(options?: RequireLoginOptions): void {
    const bypassGuard = options?.bypassLogoutGuard === true;
    if (logoutInitiated.value && !bypassGuard) return;
    if (bypassGuard) logoutInitiated.value = false;
    showLoginModal.value = true;
  }

  function hideLogin(): void {
    showLoginModal.value = false;
  }

  function setCredentialsConfigured(v: boolean): void {
    credentialsConfigured.value = !!v;
  }

  async function waitForAuthentication(): Promise<void> {
    while (!isAuthenticated.value) {
      await new Promise<void>((resolve) => setTimeout(resolve, 20));
    }
  }

  function currentSessionId(): string | undefined {
    return sessions.value.find((s) => s.current)?.id;
  }

  async function fetchSessions(): Promise<void> {
    if (!isAuthenticated.value) return;
    sessionsLoading.value = true;
    sessionsError.value = '';
    try {
      const res = await http.get('/api/auth/sessions', { validateStatus: () => true });
      if (res.status === 200 && res.data && res.data.status && Array.isArray(res.data.sessions)) {
        sessions.value = res.data.sessions;
        sessionsError.value = '';
        return;
      }
      sessionsError.value = res.data && res.data.error ? res.data.error : 'error';
    } catch (e) {
      sessionsError.value = 'error';
    } finally {
      sessionsLoading.value = false;
    }
  }

  async function revokeSession(id: string): Promise<boolean> {
    if (!id) return false;
    try {
      const res = await http.delete(`/api/auth/sessions/${id}`, { validateStatus: () => true });
      if (res.status === 200 && res.data && res.data.status) {
        sessions.value = sessions.value.filter((session) => session.id !== id);
        if (currentSessionId() === id) {
          setAuthenticated(false);
          requireLogin({ bypassLogoutGuard: true });
        }
        if (isAuthenticated.value) {
          await fetchSessions();
        }
        return true;
      }
    } catch (e) {
      /* swallow */
    }
    return false;
  }

  function isStatsOnly(): boolean {
    return isAuthenticated.value && role.value === 'stats';
  }

  function setRole(v: string): void {
    role.value = v === 'stats' ? 'stats' : 'admin';
  }

  function setOriginForbidden(allowed: string): void {
    originForbiddenAllowed.value = allowed || 'unknown';
  }

  return {
    isAuthenticated,
    ready,
    init,
    setAuthenticated,
    initiateLogout,
    onLogin,
    showLoginModal,
    requireLogin,
    hideLogin,
    credentialsConfigured,
    setCredentialsConfigured,
    credentialsLocked,
    role,
    setRole,
    isStatsOnly,
    originForbiddenAllowed,
    setOriginForbidden,
    waitForAuthentication,
    loggingIn,
    logoutInitiated,
    sessions,
    sessionsLoading,
    sessionsError,
    fetchSessions,
    revokeSession,
    currentSessionId,
  };
});
