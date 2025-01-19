/*
 * Shamelessly stolen/adapted from Simon Tatham from:
 * https://www.chiark.greenend.org.uk/~sgtatham/algorithms/listsort.c
 *
 * This file is copyright 2001 Simon Tatham.
 * 
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL SIMON TATHAM BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
static pool_item_t* sort_pool_item_list_by_address(pool_item_t *list)
{
  pool_item_t *p, *q, *e, *tail;
  int32_t insize, nmerges, psize, qsize, i;

  /*
   * Silly special case: if `list' was passed in as NULL, return
   * NULL immediately.
  */
  if (NULL == list)
    return NULL;

  insize = 1;

  while (true)
  {
    p = list;
    list = NULL;
    tail = NULL;

    nmerges = 0;  /* count number of merges we do in this pass */

    while (NULL != p)
    {
      nmerges++;  /* there exists a merge to be done */
      /* step `insize' places along from p */
      q = p;
      psize = 0;
      for (i = 0; i < insize; i++)
      {
        psize++;
        q = q->next;
        if (NULL == q)
          break;
      }

      /* if q hasn't fallen off end, we have two lists to merge */
      qsize = insize;

      /* now we have two lists; merge them */
      while (psize > 0 || (qsize > 0 && (NULL != q)))
      {
        /* decide whether next element of merge comes from p or q */
        if (psize == 0)
        {
          /* p is empty; e must come from q. */
          e = q;
          q = q->next;
          qsize--;
        } else if (qsize == 0 || !q) {
          /* q is empty; e must come from p. */
          e = p;
          p = p->next;
          psize--;
        } else if (p <= q) {
          /* First element of p is lower (or same);
          * e must come from p. */
          e = p;
          p = p->next;
          psize--;
        } else {
          /* First element of q is lower; e must come from q. */
          e = q;
          q = q->next;
          qsize--;
        }

        /* add the next element to the merged list */
        if (NULL != tail)
          tail->next = e;
        else
          list = e;

        tail = e;
      }

      /* now p has stepped `insize' places along, and q has too */
      p = q;
    }

    tail->next = NULL;

    /* If we have done only one merge, we're finished. */
    if (nmerges <= 1)   /* allow for nmerges==0, the empty list case */
      return list;

    /* Otherwise repeat, merging lists twice the size */
    insize *= 2;
  }
}

static pool_block_t* sort_pool_block_list_by_address(pool_block_t *list)
{
  pool_block_t *p, *q, *e, *tail;
  int32_t insize, nmerges, psize, qsize, i;

  /*
   * Silly special case: if `list' was passed in as NULL, return
   * NULL immediately.
  */
  if (NULL == list)
    return NULL;

  insize = 1;

  while (true)
  {
    p = list;
    list = NULL;
    tail = NULL;

    nmerges = 0;  /* count number of merges we do in this pass */

    while (NULL != p)
    {
      nmerges++;  /* there exists a merge to be done */
      /* step `insize' places along from p */
      q = p;
      psize = 0;
      for (i = 0; i < insize; i++)
      {
        psize++;
        q = q->next;
        if (NULL == q)
          break;
      }

      /* if q hasn't fallen off end, we have two lists to merge */
      qsize = insize;

      /* now we have two lists; merge them */
      while (psize > 0 || (qsize > 0 && (NULL != q)))
      {
        /* decide whether next element of merge comes from p or q */
        if (psize == 0)
        {
          /* p is empty; e must come from q. */
          e = q;
          q = q->next;
          qsize--;
        } else if (qsize == 0 || !q) {
          /* q is empty; e must come from p. */
          e = p;
          p = p->next;
          psize--;
        } else if (p <= q) {
          /* First element of p is lower (or same);
          * e must come from p. */
          e = p;
          p = p->next;
          psize--;
        } else {
          /* First element of q is lower; e must come from q. */
          e = q;
          q = q->next;
          qsize--;
        }

        /* add the next element to the merged list */
        if (NULL != tail)
          tail->next = e;
        else
          list = e;

        tail = e;
      }

      /* now p has stepped `insize' places along, and q has too */
      p = q;
    }

    tail->next = NULL;

    /* If we have done only one merge, we're finished. */
    if (nmerges <= 1)   /* allow for nmerges==0, the empty list case */
      return list;

    /* Otherwise repeat, merging lists twice the size */
    insize *= 2;
  }
}