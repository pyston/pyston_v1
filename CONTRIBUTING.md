## Contributing to Pyston

#### Pull Requests

Before a pull request can be merged, you need to to sign the [Dropbox Contributor License Agreement](https://opensource.dropbox.com/cla/).

Please make sure to run the tests (`make check` and `make check_format`) on your changes.

##### Formatting

Please make sure `make check_format` passes on your commits.  If it reports any issues, you can run `make format` to auto-apply
the project formatting rules.  Note that this will format your files in-place with no built-in undo, so you may want to
create a temporary commit if you have any unstaged changes.

### Projects

If you don't know where to start:
- Check out the list of [starter projects](https://github.com/dropbox/pyston/wiki/Starter-projects)
- Email the [pyston-dev mailing list](http://lists.pyston.org/cgi-bin/mailman/listinfo/pyston-dev), or [browse the archives](http://lists.pyston.org/pipermail/pyston-dev/)
- Join us on [#pyston](http://webchat.freenode.net/?channels=pyston) on freenode.
