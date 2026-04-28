import { useQuery } from '@tanstack/react-query';
import { api } from '../api/client';
import { queryKeys } from '../api/queryKeys';
import StickyCard from '../components/StickyCard';

export default function StickyBoard() {
  // 便签列表的唯一权威来源：服务端 /api/sticky-notes。
  // 本组件不再持有本地 store；WS 事件 sticky.upserted / sticky.deleted
  // 会由 useRealtimeSync 统一 invalidate 这条 query，触发重新 list 拉取。
  const { data: stickies, isLoading, isError, error } = useQuery({
    queryKey: queryKeys.stickies(),
    queryFn: api.listStickies,
  });

  if (isLoading) {
    return (
      <div className="flex h-full items-center justify-center text-gray-500">
        加载便签中…
      </div>
    );
  }
  if (isError) {
    return (
      <div className="flex h-full items-center justify-center text-sm text-red-600 dark:text-red-400">
        加载便签失败：{(error as Error).message}
      </div>
    );
  }
  if (!stickies || stickies.length === 0) {
    return (
      <div className="flex h-full items-center justify-center text-gray-500">
        没有便签，点击顶部“新建便签”添加一张
      </div>
    );
  }

  return (
    <div className="h-full overflow-auto p-4 thin-scroll">
      <div className="grid gap-4 grid-cols-1 md:grid-cols-2 xl:grid-cols-3">
        {stickies.map((n) => (
          <StickyCard key={n.id} sticky={n} />
        ))}
      </div>
    </div>
  );
}
