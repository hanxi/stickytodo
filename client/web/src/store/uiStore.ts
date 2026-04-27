import { create } from 'zustand';
import { persist } from 'zustand/middleware';

type DarkMode = 'system' | 'light' | 'dark';

interface UiState {
  darkMode: DarkMode;
  setDarkMode: (mode: DarkMode) => void;
  /** 当前“有效”的 darkMode（考虑 system），用于渲染时判断。 */
  isDark: () => boolean;
}

function systemPrefersDark(): boolean {
  return (
    typeof window !== 'undefined' &&
    window.matchMedia?.('(prefers-color-scheme: dark)').matches
  );
}

export const useUiStore = create<UiState>()(
  persist(
    (set, get) => ({
      darkMode: 'system',
      setDarkMode: (mode) => set({ darkMode: mode }),
      isDark: () => {
        const mode = get().darkMode;
        if (mode === 'system') {
          return systemPrefersDark();
        }
        return mode === 'dark';
      },
    }),
    {
      name: 'stickytodo.ui',
    },
  ),
);
