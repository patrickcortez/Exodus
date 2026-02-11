use std::fs::File;
use std::os::unix::io::AsRawFd;

pub const EXCON_MAGIC: u8 = b'E';

#[repr(C, packed)]
#[derive(Clone, Copy)]
pub struct ExconHeader {
    pub rows: u16,
    pub cols: u16,
    pub cursor_row: u16,
    pub cursor_col: u16,
    pub flags: u32,
    pub fg_color: u32,
    pub bg_color: u32,
    pub dirty_seq: u32,
    pub scroll_offset: u32,
    pub scroll_lines: u32,
    pub _pad: [u8; 16],
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
pub struct ExconCell {
    pub ch: u8,
    pub attr: u8,
}

#[repr(C)]
pub struct ExconCreateT {
    pub rows: u16,
    pub cols: u16,
}

#[repr(C)]
pub struct ExconInputT {
    pub len: u32,
    pub data: [u8; 256],
}

pub const FLAG_CURSOR_VISIBLE: u32 = 1 << 0;

pub const ATTR_BOLD: u8 = 1 << 3;
pub const ATTR_FG_MASK: u8 = 0x07;
pub const ATTR_BG_MASK: u8 = 0x70;
pub const ATTR_BG_SHIFT: u8 = 4;

nix::ioctl_write_ptr!(excon_create, EXCON_MAGIC, 1, ExconCreateT);
nix::ioctl_none!(excon_clear, EXCON_MAGIC, 2);
nix::ioctl_write_ptr!(excon_push_input, EXCON_MAGIC, 8, ExconInputT);
nix::ioctl_readwrite!(excon_read_input, EXCON_MAGIC, 9, ExconInputT);

pub struct KernelConsole {
    pub file: File,
    pub mmap_ptr: *mut u8,
    pub mmap_size: usize,
    pub rows: u16,
    pub cols: u16,
}

impl KernelConsole {
    pub fn open_and_create(rows: u16, cols: u16) -> Result<Self, String> {
        let file = File::options()
            .read(true)
            .write(true)
            .open("/dev/excon0")
            .map_err(|e| format!("Failed to open /dev/excon0: {}", e))?;

        let create_info = ExconCreateT { rows, cols };
        let fd = file.as_raw_fd();

        unsafe {
            excon_create(fd, &create_info)
                .map_err(|e| format!("EXCON_CREATE failed: {}", e))?;
        }

        let header_size = std::mem::size_of::<ExconHeader>();
        let cells_size = (rows as usize) * (cols as usize) * std::mem::size_of::<ExconCell>();
        let total = header_size + cells_size;
        let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) as usize };
        let mmap_size = (total + page_size - 1) & !(page_size - 1);

        let ptr = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                mmap_size,
                libc::PROT_READ,
                libc::MAP_SHARED,
                fd,
                0,
            )
        };

        if ptr == libc::MAP_FAILED {
            return Err("mmap failed".to_string());
        }

        Ok(KernelConsole {
            file,
            mmap_ptr: ptr as *mut u8,
            mmap_size,
            rows,
            cols,
        })
    }

    pub fn header(&self) -> ExconHeader {
        unsafe { std::ptr::read_unaligned(self.mmap_ptr as *const ExconHeader) }
    }

    pub fn cell(&self, row: u16, col: u16) -> ExconCell {
        let offset = std::mem::size_of::<ExconHeader>()
            + ((row as usize) * (self.cols as usize) + (col as usize))
                * std::mem::size_of::<ExconCell>();
        unsafe { std::ptr::read_unaligned(self.mmap_ptr.add(offset) as *const ExconCell) }
    }

    pub fn push_input(&self, data: &[u8]) -> Result<(), String> {
        let fd = self.file.as_raw_fd();
        let mut remaining = data;

        while !remaining.is_empty() {
            let chunk = remaining.len().min(256);
            let mut inp = ExconInputT {
                len: chunk as u32,
                data: [0u8; 256],
            };
            inp.data[..chunk].copy_from_slice(&remaining[..chunk]);

            unsafe {
                excon_push_input(fd, &inp)
                    .map_err(|e| format!("EXCON_PUSH_INPUT failed: {}", e))?;
            }
            remaining = &remaining[chunk..];
        }
        Ok(())
    }

    pub fn clear(&self) -> Result<(), String> {
        let fd = self.file.as_raw_fd();
        unsafe {
            excon_clear(fd).map_err(|e| format!("EXCON_CLEAR failed: {}", e))?;
        }
        Ok(())
    }
}

impl Drop for KernelConsole {
    fn drop(&mut self) {
        if !self.mmap_ptr.is_null() {
            unsafe {
                libc::munmap(self.mmap_ptr as *mut libc::c_void, self.mmap_size);
            }
        }
    }
}
