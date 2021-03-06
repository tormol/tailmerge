/* logmerge - A program to merge files like tail -f
 * Copyright (C) 2021 Torbjørn Birch Moltu
 *
 * licenced under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

use std::env::args_os;
use std::process::exit;
use std::fs::File;
use std::path::Path;
#[cfg(unix)]
use std::os::unix::ffi::OsStringExt;
#[cfg(wasi)]
use std::os::wasi::ffi::OsStringExt;
use std::io::{stderr, Write, Error as IoError, Read, stdout, IoSlice};
use std::error::Error as _;
use std::collections::BinaryHeap;
use std::cmp::{Ord, PartialOrd, Ordering};
use std::cell::{RefCell, Ref, Cell};
#[cfg(any(debug_assertions, feature="debug"))]
use std::fmt::{Debug, Formatter, Result as FmtResult};

fn write_all_vectored(to: &mut dyn Write,  buffers: &[IoSlice]) -> Result<(), IoError> {
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

fn error(what: &str,  path: &[u8],  e: IoError,  exit_code: i32) -> ! {
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
                        //let _ = stdout().write_all(&self.buffer[..no_newline+found+1]);
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
#[cfg(any(debug_assertions, feature="debug"))]
impl Debug for Source {
    fn fmt(&self,  fmtr: &mut Formatter) -> FmtResult {
        fmtr.debug_struct("Source")
            .field("path", &String::from_utf8_lossy(&self.path))
            .field("buffer", &String::from_utf8_lossy(&self.buffer[..self.read]))
            .field("read", &self.read)
            .finish()
    }
}

struct FirstLine<'a> {
    /// borrows a Source.buffer[self.starts_at..Source.read]
    source: Ref<'a, Source>,
    /// the first line is self.read[..self.line_length]
    line_length: usize,
    /// offset of self.read in Source.buffer
    starts_at: usize,
    source_index: usize,
    last_source: &'a Cell<usize>,
}
impl<'a> FirstLine<'a> {
    fn line(&self) -> &[u8] {
        &self.source.buffer[self.starts_at..self.starts_at+self.line_length]
    }
}
impl<'a> PartialEq for FirstLine<'a> {
    fn eq(&self,  other: &Self) -> bool {
        self.line() == other.line()
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
        match self.line().cmp(rhs.line()) {
            // invert because BinaryHeap is a max heap
            Ordering::Less => Ordering::Greater,
            Ordering::Greater => Ordering::Less,
            // prefer continuing from the same file
            Ordering::Equal if self.source_index == self.last_source.get() => Ordering::Greater,
            Ordering::Equal if rhs.source_index == self.last_source.get() => Ordering::Less,
            // make order predictable by falling back to path ordering
            Ordering::Equal if self.source_index > rhs.source_index => Ordering::Greater,
            Ordering::Equal => Ordering::Less,
        }
    }
}
#[cfg(any(debug_assertions, feature="debug"))]
impl<'a> Debug for FirstLine<'a> {
    fn fmt(&self,  fmtr: &mut Formatter) -> FmtResult {
        fmtr.debug_struct("FirstLine")
            .field("line", &String::from_utf8_lossy(self.line()))
            .field("starts_at", &self.starts_at)
            .field("read", &self.source.read)
            .field("path", &String::from_utf8_lossy(&self.source.path))
            .field("source_index", &self.source_index)
            .finish()
    }
}

fn main() {
    // open files
    let mut sources = Vec::<RefCell<Source>>::new();
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
        sources.push(RefCell::new(Source {
            path: path.into_boxed_slice(),
            file,
            buffer: vec![0; 1024*1024].into_boxed_slice(),
            read: 0,
        }));
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

    let mut first_print = true;
    let last_printed = Cell::new(sources.len());
    let stdout = stdout();
    let mut stdout = stdout.lock();

    #[cfg(feature="debug")]
    eprintln!("sources: {:?}", &sources);
    let mut sorter = BinaryHeap::<FirstLine>::with_capacity(sources.len());
    for (i, source) in sources.iter().enumerate() {
        let line = source.borrow_mut().read_next_line(0);
        if let Some(line_length) = line {
            sorter.push(FirstLine {
                source: source.borrow(),
                line_length: line_length,
                starts_at: 0,
                source_index: i,
                last_source: &last_printed,
            });
        }
    }

    // merge as many available lines as possible
    while ! sorter.is_empty() {
        let borrows = sources.iter().map(|source| source.borrow() ).collect::<Vec<_>>();
        let mut ready_output = Vec::<IoSlice>::new();
        let (source_index, written) = loop {
            #[cfg(feature="debug")]
            eprintln!("sorter before: {:?}", &sorter);

            let FirstLine { line_length, starts_at, source_index, source, .. } = sorter.pop().unwrap();
            if source_index != last_printed.get() {
                ready_output.push(IoSlice::new(&b"\n>>> "[first_print as usize..]));
                ready_output.push(IoSlice::new(&borrows[source_index].path));
                ready_output.push(IoSlice::new(b"\n"));
                #[cfg(feature="debug")] {
                    write_all_vectored(&mut stdout, &ready_output).expect("write path");
                    ready_output.clear();
                }
                last_printed.set(source_index);
                first_print = false;
            }
            #[cfg(not(feature="debug"))]
            ready_output.push(IoSlice::new(&borrows[source_index].buffer[starts_at..starts_at+line_length]));
            #[cfg(feature="debug")]
            stdout.write_all(next.line()).expect("write line");
            let after = &source.buffer[starts_at+line_length..source.read];
            if let Some(line_len) = after.iter().position(|&b| b == b'\n' ) {
                sorter.push(FirstLine {
                    source: source,
                    line_length: line_len + 1,
                    starts_at: starts_at + line_length,
                    source_index,
                    last_source: &last_printed,
                });
            } else {
                // actually write the merged lines
                if let Err(e) = write_all_vectored(&mut stdout, &ready_output) {
                    error("Error writing to", b"stdout", e, 4);
                }
                break (source_index, starts_at+line_length);
            }
            #[cfg(feature="debug")]
            eprintln!("sorter after: {:?}", &sorter);
        };
        drop(ready_output);
        drop(borrows);
        let mut source = sources[source_index].borrow_mut();
        if let Some(line_length) = source.read_next_line(written) {
            sorter.push(FirstLine {
                source: sources[source_index].borrow(),
                line_length,
                starts_at: 0,
                source_index,
                last_source: &last_printed,
            });
        } else {
            last_printed.set(sources.len());
        }
    }
}
