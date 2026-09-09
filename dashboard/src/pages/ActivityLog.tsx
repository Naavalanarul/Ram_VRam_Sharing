import { useQuery } from '@tanstack/react-query';
import { apiClient } from '../api/client';
import { useState } from 'react';
import { clsx, type ClassValue } from 'clsx';
import { twMerge } from 'tailwind-merge';

function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}

export function ActivityLog() {
  const [filter, setFilter] = useState<'ALL' | 'INFO' | 'WARN' | 'ERROR'>('ALL');
  const [search, setSearch] = useState('');

  const { data: logs = [], isLoading } = useQuery({
    queryKey: ['logs', filter, search],
    queryFn: () => apiClient.getLogs(filter === 'ALL' ? undefined : filter, search || undefined, 1),
    refetchInterval: 3000,
  });

  return (
    <div className="flex flex-col gap-4 h-full">
      <div className="flex items-center gap-4">
        <h1 className="text-xl font-semibold font-mono tracking-tight text-[var(--foreground)]">Activity Log</h1>
        <div className="flex gap-2 ml-auto">
          {(['ALL', 'INFO', 'WARN', 'ERROR'] as const).map(f => (
            <button
              key={f}
              onClick={() => setFilter(f)}
              className={cn(
                "px-3 py-1 rounded text-[10px] font-mono tracking-widest border transition-colors",
                filter === f 
                  ? "bg-[var(--foreground)] text-[#0b0f19] border-[var(--foreground)]"
                  : "bg-[var(--card)] text-[var(--foreground)] border-[var(--border)] hover:border-[var(--border-subtle)]"
              )}
            >
              {f}
            </button>
          ))}
          <input 
            type="text" 
            placeholder="Search logs..." 
            value={search}
            onChange={e => setSearch(e.target.value)}
            className="ml-2 bg-[var(--background)] border border-[var(--border)] text-[var(--foreground)] text-xs font-mono px-3 py-1.5 rounded w-64 focus:outline-none focus:border-[var(--primary)]"
          />
        </div>
      </div>

      <div className="bg-[var(--card)] border border-[var(--border)] rounded flex-1 overflow-hidden flex flex-col font-mono text-[11px]">
        <div className="grid grid-cols-12 border-b border-[var(--border-subtle)] bg-[#0d1117] p-2 font-semibold text-[var(--muted-foreground)] uppercase tracking-widest">
          <div className="col-span-2">Timestamp</div>
          <div className="col-span-1">Sev</div>
          <div className="col-span-2">Module</div>
          <div className="col-span-7">Message</div>
        </div>
        
        <div className="overflow-y-auto flex-1">
          {isLoading && <div className="p-4 text-[var(--muted-foreground)]">Loading logs...</div>}
          {!isLoading && logs.map(log => (
            <div key={log.id} className="grid grid-cols-12 border-b border-[var(--border-subtle)] p-2 hover:bg-[#161b22] items-start transition-colors">
              <div className="col-span-2 text-[var(--muted-foreground)] whitespace-nowrap">
                {new Date(log.timestamp).toISOString().replace('T', ' ').substring(0, 19)}
              </div>
              <div className="col-span-1">
                <span className={cn(
                  "px-1.5 py-0.5 rounded-sm text-[9px]",
                  log.severity === 'INFO' && "bg-[var(--healthy-bg)] text-[var(--healthy)]",
                  log.severity === 'WARN' && "bg-[var(--warning-bg)] text-[var(--warning)]",
                  log.severity === 'ERROR' && "bg-[var(--critical-bg)] text-[var(--critical)]"
                )}>
                  {log.severity}
                </span>
              </div>
              <div className="col-span-2 text-[var(--primary)]">{log.module}</div>
              <div className="col-span-7 text-[var(--foreground)] break-words">{log.message}</div>
            </div>
          ))}
          {!isLoading && logs.length === 0 && (
            <div className="p-8 text-center text-[var(--muted-foreground)]">No logs found.</div>
          )}
        </div>
      </div>
    </div>
  );
}
