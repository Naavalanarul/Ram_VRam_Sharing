import { useQuery } from '@tanstack/react-query';
import { apiClient } from '../api/client';
import { useState } from 'react';
import { clsx, type ClassValue } from 'clsx';
import { twMerge } from 'tailwind-merge';
import type { GpuJobStatus } from '../api/types';

function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}

export function GpuJobs() {
  const [filter, setFilter] = useState<GpuJobStatus | 'ALL'>('ALL');

  const { data: jobs = [], isLoading } = useQuery({
    queryKey: ['gpu-jobs'],
    queryFn: apiClient.getGpuJobs,
    refetchInterval: 3000,
  });

  if (isLoading) return <div className="text-[var(--muted-foreground)] font-mono">Loading GPU Jobs...</div>;

  const filteredJobs = jobs.filter(j => filter === 'ALL' || j.status === filter);

  return (
    <div className="flex flex-col gap-6 h-full">
      <div className="flex items-center gap-4">
        <h1 className="text-xl font-semibold font-mono tracking-tight text-[var(--compute)]">GPU Call Forwarding</h1>
        <div className="flex gap-2 ml-auto">
          {(['ALL', 'PENDING', 'RUNNING', 'COMPLETE', 'ERROR'] as const).map(f => (
            <button
              key={f}
              onClick={() => setFilter(f)}
              className={cn(
                "px-3 py-1 rounded text-[10px] font-mono tracking-widest border transition-colors",
                filter === f 
                  ? "bg-[var(--compute)] text-[#0b0f19] border-[var(--compute)]"
                  : "bg-[var(--card)] text-[var(--foreground)] border-[var(--border)] hover:border-[var(--border-subtle)]"
              )}
            >
              {f}
            </button>
          ))}
        </div>
      </div>

      <div className="bg-[var(--card)] border border-[var(--border)] rounded flex-1 overflow-hidden flex flex-col">
        <div className="grid grid-cols-6 border-b border-[var(--border-subtle)] bg-[#0d1117] p-3 text-[10px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest">
          <div className="col-span-1">ID</div>
          <div className="col-span-1">Type</div>
          <div className="col-span-1">Client Node</div>
          <div className="col-span-1">GPU Node</div>
          <div className="col-span-1">Duration</div>
          <div className="col-span-1 text-right">Status</div>
        </div>
        
        <div className="overflow-y-auto flex-1">
          {filteredJobs.map(job => (
            <div key={job.id} className="grid grid-cols-6 border-b border-[var(--border-subtle)] p-3 items-center hover:bg-[#161b22] text-xs font-mono transition-colors">
              <div className="col-span-1 text-[var(--muted-foreground)]">{job.id}</div>
              <div className="col-span-1 text-[var(--foreground)]">{job.callType}</div>
              <div className="col-span-1 text-[var(--primary)]">{job.clientNodeId}</div>
              <div className="col-span-1 text-[var(--compute)]">{job.gpuNodeId}</div>
              <div className="col-span-1 text-[var(--muted-foreground)]">{job.durationMs ? `${job.durationMs}ms` : '-'}</div>
              <div className="col-span-1 text-right">
                <span className={cn(
                  "px-2 py-0.5 rounded-[3px] text-[10px]",
                  job.status === 'RUNNING' && "bg-[var(--compute-bg)] text-[var(--compute)] border border-[var(--compute)]",
                  job.status === 'PENDING' && "bg-[var(--warning-bg)] text-[var(--warning)] border border-[var(--warning)]",
                  job.status === 'COMPLETE' && "bg-[var(--healthy-bg)] text-[var(--healthy)] border border-[var(--healthy)]",
                  job.status === 'ERROR' && "bg-[var(--critical-bg)] text-[var(--critical)] border border-[var(--critical)]"
                )}>
                  {job.status}
                </span>
              </div>
            </div>
          ))}
          {filteredJobs.length === 0 && (
            <div className="p-8 text-center text-[var(--muted-foreground)] font-mono text-xs">No jobs match filter.</div>
          )}
        </div>
      </div>
    </div>
  );
}
