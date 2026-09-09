import { useQuery } from '@tanstack/react-query';
import { apiClient } from '../../api/client';

export function Topbar() {
  const { data: localStatus } = useQuery({
    queryKey: ['localStatus'],
    queryFn: apiClient.getLocalStatus,
  });

  return (
    <header className="h-14 border-b border-[var(--border)] bg-[var(--card)] flex items-center justify-between px-6 shrink-0">
      <div className="flex items-center gap-3">
        <span className="font-mono text-xs text-[var(--muted-foreground)]">LOCAL NODE //</span>
        <span className="font-mono text-sm font-semibold text-[var(--foreground)]">
          {localStatus?.localHostname || '...'}
        </span>
        <span className="bg-[var(--border-subtle)] text-[var(--muted-foreground)] px-2 py-0.5 rounded-sm text-xs font-mono">
          {localStatus?.localNodeId ? `${localStatus.localNodeId.substring(0, 10)}...` : '...'}
        </span>
      </div>
      <div className="flex items-center gap-3">
        {localStatus?.poolJoined ? (
          <span className="border border-[var(--healthy)] bg-[var(--healthy-bg)] text-[var(--healthy)] px-2 py-1 rounded-[3px] text-[10px] font-mono flex items-center gap-1.5">
            <span className="w-1.5 h-1.5 bg-[var(--healthy)] rounded-full"></span> POOLED_SYNC
          </span>
        ) : (
          <span className="border border-[var(--muted-foreground)] bg-transparent text-[var(--muted-foreground)] px-2 py-1 rounded-[3px] text-[10px] font-mono flex items-center gap-1.5">
            <span className="w-1.5 h-1.5 bg-[var(--muted-foreground)] rounded-full"></span> ISOLATED
          </span>
        )}
      </div>
    </header>
  );
}
