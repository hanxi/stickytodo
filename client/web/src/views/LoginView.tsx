import { useState, type FormEvent } from 'react';
import { Heart } from 'lucide-react';
import { api, ApiError } from '../api/client';
import { useAuthStore } from '../store/authStore';
import SponsorModal from '../components/SponsorModal';

export default function LoginView() {
  const login = useAuthStore((s) => s.login);
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [showSponsor, setShowSponsor] = useState(false);

  async function onSubmit(e: FormEvent) {
    e.preventDefault();
    setError(null);
    setLoading(true);
    try {
      const resp = await api.login({ username, password });
      login({
        token: resp.token,
        username: resp.username,
        expiresAt: resp.expires_at,
      });
    } catch (err) {
      if (err instanceof ApiError) {
        setError(err.message);
      } else {
        setError('网络错误，请稍后再试');
      }
    } finally {
      setLoading(false);
    }
  }

  return (
    <>
      <div className="flex min-h-screen items-center justify-center bg-gray-100 px-4 dark:bg-neutral-900">
        <form
          onSubmit={onSubmit}
          className="w-full max-w-sm rounded-lg bg-white p-6 shadow dark:bg-neutral-800"
        >
          <h1 className="mb-4 text-xl font-semibold">StickyTodo · 登录</h1>
          <label className="mb-3 block text-sm">
            <span className="mb-1 block text-gray-600 dark:text-gray-300">
              用户名
            </span>
            <input
              type="text"
              value={username}
              onChange={(e) => setUsername(e.target.value)}
              className="w-full rounded border border-gray-300 bg-white px-3 py-2 text-sm focus:border-blue-500 focus:outline-none dark:border-neutral-600 dark:bg-neutral-700"
              autoFocus
              required
            />
          </label>
          <label className="mb-4 block text-sm">
            <span className="mb-1 block text-gray-600 dark:text-gray-300">
              密码
            </span>
            <input
              type="password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
              className="w-full rounded border border-gray-300 bg-white px-3 py-2 text-sm focus:border-blue-500 focus:outline-none dark:border-neutral-600 dark:bg-neutral-700"
              required
            />
          </label>
          {error ? (
            <div className="mb-3 rounded bg-red-50 px-3 py-2 text-sm text-red-700 dark:bg-red-900/30 dark:text-red-300">
              {error}
            </div>
          ) : null}
          <button
            type="submit"
            disabled={loading || !username || !password}
            className="w-full rounded bg-blue-600 py-2 text-sm font-medium text-white transition hover:bg-blue-700 disabled:cursor-not-allowed disabled:opacity-60"
          >
            {loading ? '登录中…' : '登录'}
          </button>
          <p className="mt-4 text-center text-xs text-gray-500 dark:text-gray-400">
            服务端地址取决于当前浏览器所在 host（同源请求）
          </p>
          <p className="mt-3 text-center text-xs text-gray-500 dark:text-gray-400">
            <button
              type="button"
              onClick={() => setShowSponsor(true)}
              className="inline-flex items-center gap-1 text-pink-600 hover:underline dark:text-pink-400"
            >
              <Heart size={12} /> 喜欢这个项目？赞赏支持
            </button>
          </p>
        </form>
      </div>
      <SponsorModal open={showSponsor} onClose={() => setShowSponsor(false)} />
    </>
  );
}
