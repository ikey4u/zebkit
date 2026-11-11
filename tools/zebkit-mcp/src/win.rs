//! The bounds' file checks on Windows, what unix has as O_NOFOLLOW, st_dev
//! and st_ino, st_nlink and mode 0600: through Win32, as std has no stable
//! names for them.

use std::ffi::c_void;
use std::fs::{File, OpenOptions};
use std::io;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::fs::OpenOptionsExt;
use std::os::windows::io::{AsRawHandle, FromRawHandle, RawHandle};
use std::path::Path;

const FILE_FLAG_OPEN_REPARSE_POINT: u32 = 0x0020_0000;
const FILE_FLAG_BACKUP_SEMANTICS: u32 = 0x0200_0000;
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x0000_0400;
const FILE_ATTRIBUTE_NORMAL: u32 = 0x0000_0080;
const GENERIC_WRITE: u32 = 0x4000_0000;
const CREATE_NEW: u32 = 1;
const SDDL_REVISION_1: u32 = 1;

/// BY_HANDLE_FILE_INFORMATION.
#[repr(C)]
#[derive(Default)]
struct Information {
    attributes: u32,
    creation: [u32; 2],
    access: [u32; 2],
    write: [u32; 2],
    volume_serial: u32,
    size_high: u32,
    size_low: u32,
    links: u32,
    index_high: u32,
    index_low: u32,
}

/// SECURITY_ATTRIBUTES.
#[repr(C)]
struct SecurityAttributes {
    length: u32,
    descriptor: *mut c_void,
    inherit: i32,
}

#[link(name = "kernel32")]
extern "system" {
    fn GetFileInformationByHandle(file: RawHandle, information: *mut Information) -> i32;
    fn CreateFileW(name: *const u16, access: u32, share: u32, security: *mut SecurityAttributes, disposition: u32, flags: u32,
                   template: RawHandle) -> RawHandle;
    fn LocalFree(memory: *mut c_void) -> *mut c_void;
}

#[link(name = "advapi32")]
extern "system" {
    fn ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl: *const u16, revision: u32, descriptor: *mut *mut c_void,
                                                            size: *mut u32) -> i32;
}

/// What a file is, whatever its name: its volume and index, its links, and
/// whether it is a link (a reparse point: a symbolic link or a junction).
pub struct Identity {
    pub volume: u32,
    pub index: u64,
    pub links: u32,
    pub reparse: bool,
}

pub fn identity(file: &File) -> io::Result<Identity> {
    let mut information = Information::default();
    if unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut information) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(Identity {
        volume: information.volume_serial,
        index: (u64::from(information.index_high) << 32) | u64::from(information.index_low),
        links: information.links,
        reparse: information.attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0,
    })
}

/// The identity of what is at path: a link itself, not what it points to.
pub fn identity_at(path: &Path) -> io::Result<Identity> {
    let file = OpenOptions::new()
        .access_mode(0)
        .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS)
        .open(path)?;
    identity(&file)
}

/// Opens path without following a link or junction at its end: one there is
/// opened as itself, and refused.
pub fn open_no_follow(path: &Path, write: bool, create_new: bool) -> io::Result<File> {
    let file = OpenOptions::new()
        .read(!write)
        .write(write)
        .create_new(create_new)
        .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT)
        .open(path)?;
    if identity(&file)?.reparse {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "a link"));
    }
    Ok(file)
}

/// Makes a new file at path that only its owner can open: a protected DACL
/// with one entry, full access for OWNER RIGHTS.
pub fn create_private(path: &Path) -> io::Result<File> {
    let sddl: Vec<u16> = "D:P(A;;FA;;;OW)".encode_utf16().chain(std::iter::once(0)).collect();
    let mut descriptor: *mut c_void = std::ptr::null_mut();
    if unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.as_ptr(), SDDL_REVISION_1, &mut descriptor, std::ptr::null_mut())
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    let mut attributes =
        SecurityAttributes { length: std::mem::size_of::<SecurityAttributes>() as u32, descriptor, inherit: 0 };
    let name: Vec<u16> = path.as_os_str().encode_wide().chain(std::iter::once(0)).collect();
    let handle = unsafe {
        CreateFileW(name.as_ptr(), GENERIC_WRITE, 0, &mut attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, std::ptr::null_mut())
    };
    let error = io::Error::last_os_error();
    unsafe { LocalFree(descriptor) };
    // INVALID_HANDLE_VALUE.
    if handle as isize == -1 || handle.is_null() {
        return Err(error);
    }
    Ok(unsafe { File::from_raw_handle(handle) })
}
