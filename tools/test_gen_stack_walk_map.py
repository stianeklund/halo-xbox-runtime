#!/usr/bin/env python3
"""Regression checks for transient linker-map read failures."""
import errno
import io
import unittest
from unittest.mock import Mock, call, patch

import gen_stack_walk_map as gen


class ReadMapTests(unittest.TestCase):
    def test_normal_read(self):
        with patch.object(gen, 'open', return_value=io.StringIO('first\nlast\n')):
            self.assertEqual(gen.read_map_lines('halo.map'), ['first\n', 'last\n'])

    def test_reopens_after_partial_read_failure(self):
        # Consume part of the first stream before its filesystem read fails.
        first = io.StringIO('partial\n')

        def failed_read():
            first.readline()
            raise OSError(errno.ENODATA, 'No data available')

        first.readlines = failed_read
        with patch.object(gen, 'open', side_effect=[first, io.StringIO('complete\n')]) as opening, \
                patch.object(gen.time, 'sleep') as sleeping, \
                patch.object(gen.sys, 'stderr', new_callable=io.StringIO):
            self.assertEqual(gen.read_map_lines('halo.map'), ['complete\n'])
            self.assertEqual(opening.call_args_list, [call('halo.map'), call('halo.map')])
            sleeping.assert_called_once_with(0.1)
        self.assertTrue(first.closed)

    def test_retries_open_failure(self):
        with patch.object(gen, 'open', side_effect=[
                OSError(errno.ENODATA, 'No data available'), io.StringIO('complete\n')]), \
                patch.object(gen.time, 'sleep'), \
                patch.object(gen.sys, 'stderr', new_callable=io.StringIO):
            self.assertEqual(gen.read_map_lines('halo.map'), ['complete\n'])

    def test_persistent_failure_is_bounded(self):
        error = OSError(errno.ENODATA, 'No data available')
        with patch.object(gen, 'open', side_effect=error) as opening, \
                patch.object(gen.time, 'sleep') as sleeping, \
                patch.object(gen.sys, 'stderr', new_callable=io.StringIO):
            with self.assertRaises(OSError) as caught:
                gen.read_map_lines('halo.map')
            self.assertIs(caught.exception, error)
            self.assertEqual(opening.call_count, 3)
            self.assertEqual(sleeping.call_args_list, [call(0.1), call(0.2)])

    def test_other_errors_fail_immediately(self):
        for code in (errno.ENOENT, errno.EACCES, errno.EIO):
            with self.subTest(errno=code), \
                    patch.object(gen, 'open', side_effect=OSError(code, 'read failed')) as opening, \
                    patch.object(gen.time, 'sleep') as sleeping:
                with self.assertRaises(OSError) as caught:
                    gen.read_map_lines('halo.map')
                self.assertEqual(caught.exception.errno, code)
                opening.assert_called_once_with('halo.map')
                sleeping.assert_not_called()

    def test_main_does_not_open_output_after_failed_read(self):
        with patch.object(gen.sys, 'argv', ['gen_stack_walk_map.py', 'halo.map', 'cachebeta.map']), \
                patch.object(gen.inv, 'Kb', return_value=Mock(funcs=[])), \
                patch.object(gen, 'open', side_effect=OSError(errno.ENODATA, 'No data available')) as opening, \
                patch.object(gen.time, 'sleep'), \
                patch.object(gen.sys, 'stderr', new_callable=io.StringIO):
            with self.assertRaises(OSError):
                gen.main()
            self.assertEqual(opening.call_args_list, [call('halo.map')] * 3)


if __name__ == '__main__':
    unittest.main()
