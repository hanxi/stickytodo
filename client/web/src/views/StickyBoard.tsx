import { useStickyStore } from '../store/stickyStore';
import StickyCard from '../components/StickyCard';

export default function StickyBoard() {
  const stickies = useStickyStore((s) => s.stickies);

  if (stickies.length === 0) {
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
          <StickyCard key={n.id} noteId={n.id} />
        ))}
      </div>
    </div>
  );
}
