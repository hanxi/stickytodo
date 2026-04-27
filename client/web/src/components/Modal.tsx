import { useEffect, type ReactNode } from 'react';

interface ModalProps {
  open: boolean;
  onClose: () => void;
  title?: string;
  children: ReactNode;
  footer?: ReactNode;
  maxWidth?: string;
}

export default function Modal({
  open,
  onClose,
  title,
  children,
  footer,
  maxWidth = 'max-w-lg',
}: ModalProps) {
  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onClose]);

  if (!open) return null;

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/40 p-4"
      onClick={onClose}
    >
      <div
        className={`w-full ${maxWidth} rounded-lg bg-white p-5 shadow-xl dark:bg-neutral-800 dark:text-gray-100`}
        onClick={(e) => e.stopPropagation()}
      >
        {title ? (
          <h2 className="mb-3 text-lg font-semibold">{title}</h2>
        ) : null}
        <div className="max-h-[70vh] overflow-auto thin-scroll">{children}</div>
        {footer ? (
          <div className="mt-4 flex justify-end gap-2">{footer}</div>
        ) : null}
      </div>
    </div>
  );
}
