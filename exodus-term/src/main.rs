// cargo build --release
mod config;
mod console;
mod font;
mod input;
mod renderer;

use config::Config;
use console::KernelConsole;
use renderer::Renderer;
use std::num::NonZeroU32;
use std::os::unix::io::AsRawFd;
use std::process::{Child, Command};
use std::sync::Arc;
use std::time::Duration;
use winit::application::ApplicationHandler;
use winit::event::WindowEvent;
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop};
use winit::keyboard::ModifiersState;
use winit::window::{Window, WindowAttributes, WindowId};

struct App {
    window: Option<Arc<Window>>,
    surface: Option<softbuffer::Surface<Arc<Window>, Arc<Window>>>,
    console: Option<KernelConsole>,
    renderer: Option<Renderer>,
    config: Config,
    modifiers: ModifiersState,
    shell_process: Option<Child>,
}

impl App {
    fn new() -> Self {
        App {
            window: None,
            surface: None,
            console: None,
            renderer: None,
            config: Config::default(),
            modifiers: ModifiersState::empty(),
            shell_process: None,
        }
    }

    fn spawn_shell(&mut self) {
        let console = match self.console.as_ref() {
            Some(c) => c,
            None => return,
        };

        let fd = console.file.as_raw_fd();

        unsafe {
            let flags = libc::fcntl(fd, libc::F_GETFD);
            if flags >= 0 {
                libc::fcntl(fd, libc::F_SETFD, flags & !libc::FD_CLOEXEC);
            }
        }

        let exe = std::env::current_exe().unwrap_or_default();
        let project_dir = exe
            .parent()
            .and_then(|p| p.parent())
            .and_then(|p| p.parent())
            .and_then(|p| p.parent())
            .unwrap_or(std::path::Path::new("."));

        let exodus_path = project_dir.join("bin").join("exodus");
        if !exodus_path.exists() {
            eprintln!("exodus binary not found at {:?}", exodus_path);
            return;
        }

        let fd_str = fd.to_string();
        let child = Command::new(&exodus_path)
            .arg("--shell")
            .env("EXODUS_EXCON", "1")
            .env("EXCON_FD", &fd_str)
            .spawn();

        match child {
            Ok(c) => {
                eprintln!("Spawned exodus shell (pid {}, excon_fd={})", c.id(), fd);
                self.shell_process = Some(c);
            }
            Err(e) => eprintln!("Failed to spawn exodus: {}", e),
        }
    }
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        let cfg = &self.config;

        let rend = Renderer::new(cfg.font_size);
        let (pw, ph) = rend.pixel_size(cfg.default_cols, cfg.default_rows);

        let attrs = WindowAttributes::default()
            .with_title("Exodus Terminal")
            .with_inner_size(winit::dpi::PhysicalSize::new(pw, ph));

        let window = Arc::new(event_loop.create_window(attrs).expect("Failed to create window"));

        let context =
            softbuffer::Context::new(window.clone()).expect("Failed to create softbuffer context");
        let surface = softbuffer::Surface::new(&context, window.clone())
            .expect("Failed to create surface");

        match KernelConsole::open_and_create(cfg.default_rows, cfg.default_cols) {
            Ok(con) => {
                self.console = Some(con);
                self.spawn_shell();
            }
            Err(e) => {
                eprintln!("FATAL: Cannot open kernel console: {}", e);
                eprintln!("Make sure exodus_console.ko is loaded:");
                eprintln!("  sudo insmod k-module/exodus_console/exodus_console.ko");
                eprintln!("  sudo chmod 666 /dev/excon0");
                event_loop.exit();
                return;
            }
        }

        self.window = Some(window);
        self.surface = Some(surface);
        self.renderer = Some(rend);

        event_loop.set_control_flow(ControlFlow::Poll);
    }

    fn window_event(
        &mut self,
        event_loop: &ActiveEventLoop,
        _window_id: WindowId,
        event: WindowEvent,
    ) {
        match event {
            WindowEvent::CloseRequested => {
                if let Some(mut child) = self.shell_process.take() {
                    let _ = child.kill();
                }
                event_loop.exit();
            }

            WindowEvent::ModifiersChanged(mods) => {
                self.modifiers = mods.state();
            }

            WindowEvent::KeyboardInput { event, .. } => {
                if let Some(ref console) = self.console {
                    let ctrl = self.modifiers.control_key();
                    if let Some(bytes) = input::translate_key(&event.logical_key, event.state, ctrl)
                    {
                        let _ = console.push_input(&bytes);
                    }
                }
            }

            WindowEvent::Resized(size) => {
                if let Some(ref mut surface) = self.surface {
                    let _ = surface.resize(
                        NonZeroU32::new(size.width).unwrap_or(NonZeroU32::new(1).unwrap()),
                        NonZeroU32::new(size.height).unwrap_or(NonZeroU32::new(1).unwrap()),
                    );
                }
            }

            WindowEvent::RedrawRequested => {
                if let (Some(ref mut surface), Some(ref mut renderer), Some(ref console)) =
                    (&mut self.surface, &mut self.renderer, &self.console)
                {
                    let (pw, ph) = renderer.pixel_size(console.cols, console.rows);
                    let _ = surface.resize(
                        NonZeroU32::new(pw).unwrap_or(NonZeroU32::new(1).unwrap()),
                        NonZeroU32::new(ph).unwrap_or(NonZeroU32::new(1).unwrap()),
                    );

                    if let Ok(mut buffer) = surface.buffer_mut() {
                        renderer.render(console, &mut buffer, pw);
                        let _ = buffer.present();
                    }
                }

                if let Some(ref window) = self.window {
                    window.request_redraw();
                }
            }

            _ => {}
        }
    }

    fn about_to_wait(&mut self, _event_loop: &ActiveEventLoop) {
        if let Some(ref window) = self.window {
            window.request_redraw();
        }
        std::thread::sleep(Duration::from_millis(self.config.poll_interval_ms));
    }
}

fn main() {
    let event_loop = EventLoop::new().expect("Failed to create event loop");
    let mut app = App::new();
    let _ = event_loop.run_app(&mut app);
}
