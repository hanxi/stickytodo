import { Github, Heart } from 'lucide-react';
import Modal from './Modal';

interface SponsorModalProps {
  open: boolean;
  onClose: () => void;
}

/**
 * 赞赏支持弹窗。内容与 README 的「支持项目」章节保持一致，三项：
 *   1. GitHub Star 项目链接
 *   2. 爱发电赞赏链接
 *   3. 扫码二维码（图片走 Vite public/ → `/app/sponsor-qrcode.png`）
 *
 * 复用点：
 *   - AppBar：已登录状态下右上角「赞赏」按钮触发
 *   - LoginView：未登录登录页底部「喜欢这个项目？赞赏支持」按钮触发
 *
 * 二维码引用用相对路径 `sponsor-qrcode.png`（不带前导 `/`），由浏览器按当前
 * 页相对解析：index.html 挂在 `/app/`，实际请求 `/app/sponsor-qrcode.png`，
 * 与 Vite `base='/app/'` 发布路径一致，且满足 CSP `img-src 'self'`。
 */
export default function SponsorModal({ open, onClose }: SponsorModalProps) {
  return (
    <Modal open={open} onClose={onClose} title="💖 支持项目">
      <p className="mb-4 text-sm text-gray-600 dark:text-gray-300">
        如果这个项目对你有帮助，欢迎通过以下方式支持：
      </p>
      <ul className="mb-4 space-y-2 text-sm">
        <li className="flex items-start gap-2">
          <Github size={16} className="mt-0.5 flex-shrink-0" />
          <span>
            <strong>⭐ Star 项目</strong>：
            <a
              href="https://github.com/hanxi/stickytodo"
              target="_blank"
              rel="noopener noreferrer"
              className="ml-1 text-blue-600 hover:underline dark:text-blue-400"
            >
              github.com/hanxi/stickytodo
            </a>
          </span>
        </li>
        <li className="flex items-start gap-2">
          <Heart size={16} className="mt-0.5 flex-shrink-0 text-red-500" />
          <span>
            <strong>💝 爱发电</strong>：
            <a
              href="https://afdian.com/a/imhanxi"
              target="_blank"
              rel="noopener noreferrer"
              className="ml-1 text-blue-600 hover:underline dark:text-blue-400"
            >
              afdian.com/a/imhanxi
            </a>
          </span>
        </li>
      </ul>
      <div className="flex flex-col items-center gap-2">
        <p className="text-xs text-gray-500 dark:text-gray-400">
          或扫码请作者喝杯奶茶 ☕
        </p>
        <img
          src="sponsor-qrcode.png"
          alt="赞赏码"
          className="h-64 w-64 rounded border border-gray-200 dark:border-neutral-700"
        />
      </div>
    </Modal>
  );
}
