# LogMerger

A simple command line utility for merging files the way `tail -f` does.
It differs from `sort -m` in that the file name is printed above each block of lines from one file.
Lines from the same file are guarenteed to be printed in order.

## Example

```sh
$ seq 1 6 > foo.lst
$ seq 4 9 > bar.lst
$ logmerger foo.lst bar.lst
>>> foo.lst
1
2
3
4

>>> bar.lst
4
5

>>> foo.lst
5
6

>>> bar.lst
6
7
8
9
$ rm foo.lst bar.lst
```

## Optimizations

* Because it doesn't need to sort the entire file, memory usage is reduced.
* Uses vectored I/O to avoid copying lines while reducing the number of syscall.

## Limitations

* Haven't been tested with files that aren't read in one go.
* Haven't been tested with lines long enough to require growing the buffer.
* Doesn't do locale-aware sorting.

## License

Copyright 2021 Torbjørn Birch Moltu

Licensed under the terms of the GNU Lesser General Public License
as published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally
submitted for inclusion in the work by you, shall be licensed as above,
without any additional terms or conditions.
