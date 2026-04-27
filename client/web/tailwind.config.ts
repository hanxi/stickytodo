import type { Config } from 'tailwindcss';

/**
 * Tailwind 主题与 macOS 客户端 CodableRGBA 的 5 种预设颜色保持一致。
 * darkMode: 'class' 让 App.tsx 的 useEffect 直接切换 documentElement.classList。
 */
const config: Config = {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  darkMode: 'class',
  theme: {
    extend: {
      colors: {
        sticky: {
          yellow: '#FFEB8A',
          green: '#C8F1C8',
          blue: '#C6E3FF',
          pink: '#FFD1DC',
          purple: '#E1D4FF',
        },
      },
      fontFamily: {
        sans: [
          '-apple-system',
          'BlinkMacSystemFont',
          'Segoe UI',
          'Roboto',
          'Helvetica Neue',
          'sans-serif',
        ],
      },
    },
  },
  plugins: [],
};

export default config;
