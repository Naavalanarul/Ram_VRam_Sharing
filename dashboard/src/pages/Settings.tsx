import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { apiClient } from '../api/client';
import { useState, useEffect } from 'react';
import type { DaemonConfig } from '../api/types';

export function Settings() {
  const queryClient = useQueryClient();
  const { data: config, isLoading } = useQuery({
    queryKey: ['config'],
    queryFn: apiClient.getConfig,
  });

  const [form, setForm] = useState<DaemonConfig | null>(null);

  useEffect(() => {
    if (config) setForm(config);
  }, [config]);

  const mutation = useMutation({
    mutationFn: (newConfig: DaemonConfig) => apiClient.updateConfig(newConfig),
    onSuccess: (updated) => {
      queryClient.setQueryData(['config'], updated);
      alert('Config updated successfully.');
    }
  });

  if (isLoading || !form) return <div className="text-[var(--muted-foreground)] font-mono">Loading config...</div>;

  const handleSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    mutation.mutate(form);
  };

  return (
    <div className="flex flex-col gap-6 max-w-2xl">
      <h1 className="text-xl font-semibold font-mono tracking-tight text-[var(--foreground)]">Daemon Configuration</h1>
      
      <form onSubmit={handleSubmit} className="bg-[var(--card)] border border-[var(--border)] rounded flex flex-col">
        <div className="p-4 border-b border-[var(--border-subtle)] flex flex-col gap-4">
          
          <div className="flex flex-col gap-1.5">
            <label className="text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest flex items-center justify-between">
              Slab Percentage
              <span className="bg-[var(--healthy-bg)] text-[var(--healthy)] px-1.5 py-0.5 rounded-sm text-[9px]">HOT RELOAD</span>
            </label>
            <div className="flex items-center gap-2">
              <input 
                type="number" 
                value={form.slabPercentage} 
                onChange={e => setForm({...form, slabPercentage: Number(e.target.value)})}
                className="bg-[#0b0f19] border border-[var(--border)] text-[var(--foreground)] text-xs font-mono px-3 py-2 rounded focus:outline-none focus:border-[var(--primary)] flex-1"
              />
              <span className="text-[var(--muted-foreground)] font-mono text-xs">%</span>
            </div>
          </div>

          <div className="flex flex-col gap-1.5">
            <label className="text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest flex items-center justify-between">
              Safety Floor (MB)
              <span className="bg-[var(--healthy-bg)] text-[var(--healthy)] px-1.5 py-0.5 rounded-sm text-[9px]">HOT RELOAD</span>
            </label>
            <input 
              type="number" 
              value={form.safetyFloorMb} 
              onChange={e => setForm({...form, safetyFloorMb: Number(e.target.value)})}
              className="bg-[#0b0f19] border border-[var(--border)] text-[var(--foreground)] text-xs font-mono px-3 py-2 rounded focus:outline-none focus:border-[var(--primary)]"
            />
          </div>

          <div className="flex flex-col gap-1.5">
            <label className="text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest flex items-center justify-between">
              Cache Size (MB)
              <span className="bg-[var(--warning-bg)] text-[var(--warning)] px-1.5 py-0.5 rounded-sm text-[9px]">RESTART REQUIRED</span>
            </label>
            <input 
              type="number" 
              value={form.cacheSizeMb} 
              onChange={e => setForm({...form, cacheSizeMb: Number(e.target.value)})}
              className="bg-[#0b0f19] border border-[var(--border)] text-[var(--foreground)] text-xs font-mono px-3 py-2 rounded focus:outline-none focus:border-[var(--primary)]"
            />
            <span className="text-[10px] text-[var(--muted-foreground)] font-mono">Changes to cache size require restarting memoryd.</span>
          </div>

          <div className="flex flex-col gap-1.5">
            <label className="text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest flex items-center justify-between">
              Peer Timeout (ms)
              <span className="bg-[var(--healthy-bg)] text-[var(--healthy)] px-1.5 py-0.5 rounded-sm text-[9px]">HOT RELOAD</span>
            </label>
            <input 
              type="number" 
              value={form.peerTimeoutMs} 
              onChange={e => setForm({...form, peerTimeoutMs: Number(e.target.value)})}
              className="bg-[#0b0f19] border border-[var(--border)] text-[var(--foreground)] text-xs font-mono px-3 py-2 rounded focus:outline-none focus:border-[var(--primary)]"
            />
          </div>

        </div>
        <div className="p-4 bg-[#0b0f19] flex justify-end">
          <button 
            type="submit" 
            disabled={mutation.isPending}
            className="bg-[var(--primary)] text-[#0b0f19] font-mono text-xs font-bold px-4 py-2 rounded hover:opacity-90 disabled:opacity-50 transition-opacity"
          >
            {mutation.isPending ? 'SAVING...' : 'SAVE CONFIG'}
          </button>
        </div>
      </form>
    </div>
  );
}
