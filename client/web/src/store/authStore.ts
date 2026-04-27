import { create } from 'zustand';
import { persist } from 'zustand/middleware';

interface AuthState {
  token: string | null;
  username: string | null;
  expiresAt: string | null; // RFC3339
  login: (payload: { token: string; username: string; expiresAt: string }) => void;
  logout: () => void;
  /** 启动时调用：过期则清空 */
  hydrateCheckExpiry: () => void;
}

export const useAuthStore = create<AuthState>()(
  persist(
    (set, get) => ({
      token: null,
      username: null,
      expiresAt: null,
      login: ({ token, username, expiresAt }) =>
        set({ token, username, expiresAt }),
      logout: () => set({ token: null, username: null, expiresAt: null }),
      hydrateCheckExpiry: () => {
        const exp = get().expiresAt;
        if (!exp) {
          return;
        }
        const expMs = Date.parse(exp);
        if (Number.isFinite(expMs) && expMs <= Date.now()) {
          set({ token: null, username: null, expiresAt: null });
        }
      },
    }),
    {
      name: 'stickytodo.auth',
    },
  ),
);
