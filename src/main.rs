use std::env::args_os;
use std::process::exit;
use std::fs::File;
use std::path::Path;
#[cfg(unix)]
use std::os::unix::ffi::OsStringExt;
#[cfg(wasi)]
use std::os::wasi::ffi::OsStringExt;
use std::io::{stderr, Write, Error, Read, stdout, IoSlice};
use std::error::Error as _;
use std::collections::BinaryHeap;
use std::ops::Range;
use std::cmp::{Ord, PartialOrd, Ordering};

fn write_all_vectored(to: &mut dyn Write,  buffers: &[IoSlice]) -> Result<(), Error> {
    let mut i = 0;
    while i < buffers.len() {
        let mut wrote = to.write_vectored(&buffers[i..])?;
        while wrote > buffers[i].len() {
            wrote -= buffers[i].len();
            i += 1;
        }
        if wrote != 0 {
            to.write_all(&buffers[i][wrote..])?;
            i += 1;
        }
    }
    Ok(())
}

fn error(what: &str,  path: &[u8],  e: Error,  exit_code: i32) -> ! {
    let stderr = stderr();
    let _ = write_all_vectored(&mut stderr.lock(), &[
        IoSlice::new(what.as_bytes()),
        IoSlice::new(b" "),
        IoSlice::new(path),
        IoSlice::new(b": "),
        #[allow(deprecated)]
        IoSlice::new(e.description().as_bytes()),
        IoSlice::new(b"\n"),
    ]);
    exit(exit_code);
}

struct Source {
    path: Box<[u8]>,
    file: File,
    buffer: Box<[u8]>,
    read: usize,
}

impl Source {
    /// Returns None on EOF and the length of the next line otherwise.
    pub fn read_next_line(&mut self,  next_line_begins: usize) -> Option<usize> {
        self.buffer.copy_within(next_line_begins..self.read, 0);
        self.read -= next_line_begins;
        loop {
            match self.file.read(&mut self.buffer[self.read..]) {
                Ok(new_bytes @ 1..=usize::MAX) => {
                    let no_newline = self.read;
                    self.read += new_bytes;
                    let new_part = &self.buffer[no_newline..self.read];
                    if let Some(found) = new_part.iter().position(|&b| b == b'\n' ) {
                        return Some(no_newline+found+1);
                    } else if self.buffer.len() - self.read < self.buffer.len() / 4 {
                        let mut new = Vec::with_capacity(self.buffer.len()*2);
                        new.extend_from_slice(&self.buffer[..self.read]);
                        new.truncate(self.buffer.len()*2);
                        self.buffer = new.into_boxed_slice();
                    }
                    // continue
                }
                Ok(0) if self.read == 0 => {// EOF reached after a newline
                    return None;
                }
                Ok(0) => {// no newline at end of file; add one
                    if self.read < self.buffer.len() {
                        self.buffer[self.read] = b'\n';
                    } else {
                        let mut new = Vec::with_capacity(self.buffer.len()+1);
                        new.extend_from_slice(&self.buffer);
                        new.push(b'\n');
                        self.buffer = new.into_boxed_slice();
                    }
                    self.read += 1;
                    return Some(self.read);
                }
                Err(e) => error("Error reading from", &self.path, e, 3),
                Ok(negative) => unreachable!("usize value not in 0..=usize::MAX: {}", negative),
            }
        }
    }
}

struct FirstLine<'a> {
    /// borrows a Source.buffer[self.starts_at..Source.read]
    read: &'a [u8],
    /// the first line is self.read[..self.line_length]
    line_length: usize,
    /// offset of self.read in Source.buffer
    starts_at: usize,
    /// index of the source
    source: usize,
}
impl<'a> PartialEq for FirstLine<'a> {
    fn eq(&self,  other: &Self) -> bool {
        &self.read[..self.line_length] == &other.read[..other.line_length]
    }
}
impl<'a> Eq for FirstLine<'a> {}
impl<'a> PartialOrd for FirstLine<'a> {
    fn partial_cmp(&self,  rhs: &Self) -> Option<Ordering> {
        Some(self.cmp(rhs))
    }
}
impl<'a> Ord for FirstLine<'a> {
    fn cmp(&self,  rhs: &Self) -> Ordering {
        rhs.read[..rhs.line_length].cmp(&self.read[..self.line_length])
    }
}

fn main() {
    // open files
    let mut sources = Vec::<Source>::new();
    for arg in args_os().skip(1) {
        // open the file before converting the OsString to bytes
        let file_result = File::open(Path::new(&arg));
        #[cfg(any(unix, wasi))]
        let path = arg.into_vec();
        #[cfg(not(any(unix, wasi)))]
        let path = arg.into_string()
            .unwrap_or_else(|bad| bad.to_string_lossy().into_owned() )
            .into_bytes();
        // handle potential error now that we have the path as bytes
        let file = file_result.unwrap_or_else(|err| {
            error("Cannot open", &path, err, 2);
        });
        sources.push(Source {
            path: path.into_boxed_slice(),
            file,
            buffer: vec![0; 1024*1024].into_boxed_slice(),
            read: 0,
        });
    }

    if sources.is_empty() {
        eprintln!("Usage: log_merge file1 [file2]...");
        eprintln!();
        eprintln!("\"Sorts\" the files but prints the file name above each group of lines from a file, like `tail -f`.");
        eprintln!("Files are merged by sorting the next unprinted line from each file,");
        eprintln!("without reordering lines from the same file or keeping everything in RAM.");
        eprintln!("(Memory usage is linear with the number of files, not with the file sizes.)");
        exit(1);
    }

    let mut next_line: Vec::<Range<usize>> = vec![0..0; sources.len()];
    for i in (0..sources.len()).rev() {
        if let Some(line_len) = sources[i].read_next_line(0) {
            next_line[i] = 0..line_len;
        } else {
            next_line.swap_remove(i);
            sources.swap_remove(i);
        }
    }

    let mut has_printed = false;
    let mut last_printed = sources.len();
    let stdout = stdout();
    let mut stdout = stdout.lock();
    while ! sources.is_empty() {
        let mut sorter = BinaryHeap::<FirstLine>::with_capacity(sources.len());
        for (i, line) in next_line.iter_mut().enumerate() {
            sorter.push(FirstLine {
                read: &sources[i].buffer[line.clone()],
                line_length: line.end - line.start,
                starts_at: line.start,
                source: i,
            });
        }

        // merge as many available lines as possible
        let mut ready_output = Vec::<IoSlice>::new();
        let (needs_more, written) = loop {
            let mut next = sorter.pop().unwrap();
            if next.source != last_printed {
                if has_printed {
                    ready_output.push(IoSlice::new(b"\n>>> "));
                } else {
                    ready_output.push(IoSlice::new(b">>> "));
                    has_printed = true;
                }
                ready_output.push(IoSlice::new(&sources[next.source].path));
                ready_output.push(IoSlice::new(b"\n"));
                last_printed = next.source;
            }
            let (this_line, after) = next.read.split_at(next.line_length);
            ready_output.push(IoSlice::new(this_line));
            next.starts_at += this_line.len();
            if let Some(line_len) = after.iter().position(|&b| b == b'\n' ) {
                next.read = after;
                next.line_length = line_len + 1;
                sorter.push(next);
            } else {
                break (next.source, next.starts_at);
            }
        };
        // empty the next line information into next_line, so that the borrow of source expires
        for line in sorter {
            next_line[line.source] = line.starts_at..line.starts_at+line.line_length;
        }
        // actually write the merged lines
        if let Err(e) = write_all_vectored(&mut stdout, &ready_output) {
            error("Error writing to", b"stdout", e, 4);
        }

        if let Some(line_length) = sources[needs_more].read_next_line(written) {
            next_line[needs_more] = 0..line_length;
        } else {
            // everything printed, close file
            sources.swap_remove(needs_more);
            next_line.swap_remove(needs_more);
            last_printed = sources.len();
        }
    }
}
