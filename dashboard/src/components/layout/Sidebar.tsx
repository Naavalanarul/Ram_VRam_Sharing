import { NavLink } from 'react-router-dom';
import { clsx, type ClassValue } from 'clsx';
import { twMerge } from 'tailwind-merge';

function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}

export function Sidebar() {
  const links = [
    { name: 'Cluster Overview', path: '/' },
    { name: 'Peer Detail', path: '/peers' },
    { name: 'GPU Jobs', path: '/gpu', badge: '3 RUNNING' },
    { name: 'Settings', path: '/settings' },
    { name: 'Activity Log', path: '/logs' },
  ];

  return (
    <aside className="w-60 border-r border-[var(--border)] bg-[var(--card)] flex flex-col p-4 shrink-0">
      <div className="font-mono font-bold text-lg text-[var(--primary)] tracking-tight mb-6">
        SWARM_MEM // v2.4.1
      </div>
      
      <nav className="flex flex-col gap-2">
        {links.map((link) => (
          <NavLink
            key={link.path}
            to={link.path}
            className={({ isActive }) =>
              cn(
                'px-3 py-2 rounded text-xs font-semibold tracking-wide uppercase flex justify-between items-center transition-colors',
                isActive
                  ? 'bg-[var(--primary)] text-[#0b0f19]'
                  : 'text-[var(--muted-foreground)] hover:text-[var(--foreground)] hover:bg-[#161b22]'
              )
            }
          >
            {link.name}
            {link.badge && (
              <span className="bg-[var(--compute-bg)] text-[var(--compute)] px-1.5 py-0.5 rounded-sm text-[10px]">
                {link.badge}
              </span>
            )}
          </NavLink>
        ))}
      </nav>

      <div className="mt-auto p-3 border border-[var(--border-subtle)] rounded bg-[#161b22] flex flex-col gap-2">
        <div className="text-xs font-semibold text-[var(--muted-foreground)] uppercase tracking-wider">
          Bridge Status
        </div>
        <div className="flex items-center gap-2">
          <div className="w-2 h-2 rounded-full bg-[var(--healthy)] animate-pulse"></div>
          <span className="font-mono text-xs text-[var(--healthy)]">CONNECTED</span>
        </div>
      </div>
    </aside>
  );
}
