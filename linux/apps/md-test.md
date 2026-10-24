The editor's reader
===================

A test page for `nxedit --read` (`tools/test_all.py nxedit-read`): every kind of thing the
reader shows. Its sum is the one `tools/web_test.c --sections` gives on the host.

## Text

Plain text, *emphasis*, **strong**, ***both***, ~~struck~~, `code` and ``a `tick` inside``.
A line with a hard break  
after two spaces, and a [link to a heading](#lists-and-quotes), a [file](doc/shaderemu/README.md)
and <https://example.com/>, then a bare https://example.org/path too.

Escapes: \*not emphasis\*, 2 * 3 * 4, snake_case_name and a_b.

Setext second level
-------------------

## Lists and quotes

- one
- two, with *emphasis*
  - nested
  - and again

    with a second paragraph
- three

1. first
2. second
3. third

> A quote with **strong** text,
continued lazily.
>
> > and a quote inside it.

## Code

    four spaces of indent
    make a block

```c
int
main(void)
{
	return 0;	/* a tab */
}
```

~~~
a fence of tildes, with ``` inside
~~~

## Tables

| Left | Centre | Right |
|:-----|:------:|------:|
| a | b | 1 |
| `code` | *em* | 22 |
| a \| pipe | | 333 |

---

### A third level

#### A fourth

##### A fifth

###### A sixth

![a picture](picture.png) is shown by its text.

The end.
