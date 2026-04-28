// 把 WebSocket 实时事件桥接到 React Query 的 cache 失效机制。
//
// 职责：
//   - 监听 authStore.token 变化：有 token 就 connect，没 token 就 disconnect
//   - 收到业务事件按约定把对应的 queryKey 失效（靠 React Query 自己再去 refetch）
//   - 收到连接级信号：
//       * reconnected → 一次性 invalidate 所有相关 cache（断线期间可能错过事件，需全量拉）
//       * unauthorized → token 失效，调用 authStore.logout() 跳回登录页
//
// 与 mutation 自身的 onSuccess 的分工：
//   - 本端自己发起的写入（比如本 tab 修改标题）：由 mutation 的 onSuccess 用
//     服务端响应写回 cache，获得即时且权威的结果。**不依赖** WS 兜底——因为
//     后端 hub 会把事件也回推给发起端（当前实现不区分 sender），如果让 WS
//     也去 invalidate，会和 mutation 的 Promise 发生竞速且多一次完整 list 拉取。
//   - 其他端发起的写入（同账号另一个 tab / macOS 客户端）：本端 mutation 不会
//     响应，WS 事件是唯一通知途径；本 hook 负责 invalidate 让列表重新拉取。
//   - 断线重连：无法保证断开期间错过多少事件，只能全量 invalidate 强制重拉。
//
// 设计约束：
//   - 本 hook 必须在 <QueryClientProvider> 作用域内使用（依赖 useQueryClient）
//   - 每个 App 实例只应挂一次（挂在根 App 组件），避免多次 onEvent 注册导致重复 invalidate
//   - 不负责错误展示：底层 WS 异常、重连次数等"状态"留给后续 uiStore / 组件 poll isConnected 自取

import { useEffect } from 'react';
import { useQueryClient } from '@tanstack/react-query';
import { useAuthStore } from '../store/authStore';
import { stickyWS, type WSEvent } from '../api/ws';

/**
 * 事件类型 → 需要失效的 queryKey 前缀。
 * 前缀失效语义：React Query 的 invalidateQueries 会匹配所有以这个前缀开头的 key，
 * 例如 ['todos'] 会命中 ['todos', filter1] / ['todos', filter2] 等所有便签的 todo 列表。
 *
 * 与 ws/event.go 的事件类型常量一一对应：
 *   - todo.*   → todos 列表 + tags 汇总（create/delete 会改变 tag 分布）
 *   - sticky.* → stickies 列表
 */
const EVENT_INVALIDATE_MAP: Record<string, ReadonlyArray<ReadonlyArray<unknown>>> = {
  'todo.created': [['todos'], ['tags']],
  'todo.updated': [['todos']],
  'todo.deleted': [['todos'], ['tags']],
  'sticky.upserted': [['stickies']],
  'sticky.deleted': [['stickies']],
};

/**
 * 在 App 根挂载一次，完成 WS 连接 + 事件桥接。
 * 无返回值：所有副作用都在 useEffect 内管理，卸载时自动清理订阅和连接。
 */
export function useRealtimeSync(): void {
  const queryClient = useQueryClient();
  const token = useAuthStore((s) => s.token);
  const logout = useAuthStore((s) => s.logout);

  // 连接生命周期：token 变化时重连；无 token 时断开
  useEffect(() => {
    if (!token) {
      stickyWS.disconnect();
      return;
    }
    // baseURL 不传 → 单例内部会取 window.location.origin（同源）
    stickyWS.connect(token);
    // 注意不要在 cleanup 里 disconnect：如果只是 token 未变（re-render 触发的 effect 重跑），
    // 单例内部会因为参数不变直接短路返回；而"登出"场景会走到上面的 !token 分支主动断开。
    // 组件真正卸载场景（比如 HMR）下，保留连接是无害的——下一次挂载会 idempotent reuse。
    return undefined;
  }, [token]);

  // 业务事件桥接：event.type → invalidate 对应 queryKey 前缀
  useEffect(() => {
    const off = stickyWS.onEvent((event: WSEvent) => {
      const keys = EVENT_INVALIDATE_MAP[event.type];
      if (!keys) {
        // 未知事件类型：后端未来可能新增，这里不报错也不拒绝，留出向前兼容空间
        return;
      }
      for (const key of keys) {
        // predicate 前缀匹配会比直接用 queryKey 参数更安全：
        // 后者要求完全相等，前者是"以这个数组为前缀"，符合我们的分层 key 设计
        queryClient.invalidateQueries({
          predicate: (q) => isKeyPrefix(q.queryKey, key),
        });
      }
    });
    return off;
  }, [queryClient]);

  // 连接信号桥接
  useEffect(() => {
    const off = stickyWS.onSignal((signal) => {
      if (signal === 'reconnected') {
        // 断线期间可能错过事件：一次性刷掉所有业务 cache
        queryClient.invalidateQueries({
          predicate: (q) =>
            isKeyPrefix(q.queryKey, ['todos']) ||
            isKeyPrefix(q.queryKey, ['tags']) ||
            isKeyPrefix(q.queryKey, ['stickies']),
        });
        return;
      }
      if (signal === 'unauthorized') {
        // token 失效：登出（authStore 会自动清 localStorage）
        logout();
      }
      // 'ready' 和 'disconnected' 不需要做 cache 操作；UI 若需展示连接状态
      // 可另外订阅单例的 onSignal
    });
    return off;
  }, [queryClient, logout]);
}

/**
 * 判断 candidate 是否以 prefix 为前缀（按元素浅比较）。
 * React Query 的 queryKey 元素允许是 object（比如 TodoFilter），这种场景我们
 * 只关心第一段字符串前缀是否匹配，对象部分不在比较范围——因为 EVENT_INVALIDATE_MAP
 * 里的前缀都是单字符串元素数组。
 */
function isKeyPrefix(
  candidate: ReadonlyArray<unknown>,
  prefix: ReadonlyArray<unknown>,
): boolean {
  if (candidate.length < prefix.length) return false;
  for (let i = 0; i < prefix.length; i++) {
    if (candidate[i] !== prefix[i]) return false;
  }
  return true;
}
