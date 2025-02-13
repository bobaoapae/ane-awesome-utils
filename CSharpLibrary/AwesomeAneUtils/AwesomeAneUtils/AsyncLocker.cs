using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace AwesomeAneUtils;

public class AsyncLocker : IDisposable
{
    private readonly SemaphoreSlim _semaphore = new(1, 1);

    private readonly Guid _id;
    private bool _isDisposed;

    public AsyncLocker()
    {
        _id = Guid.NewGuid();
    }

    public async Task<IDisposable> LockAsync()
    {
        await _semaphore.WaitAsync();
        return new Releaser(_semaphore);
    }

    public IDisposable Lock()
    {
        _semaphore.Wait();
        return new Releaser(_semaphore);
    }

    public static async Task<IDisposable> LockAsync(AsyncLocker locker1, AsyncLocker locker2)
    {
        if (locker1._id > locker2._id)
        {
            await locker2._semaphore.WaitAsync();
            await locker1._semaphore.WaitAsync();

            return new ReleaserTwo(locker2._semaphore, locker1._semaphore);
        }

        await locker1._semaphore.WaitAsync();
        await locker2._semaphore.WaitAsync();

        return new ReleaserTwo(locker1._semaphore, locker2._semaphore);
    }

    public static async Task<IDisposable> LockAsync(AsyncLocker locker1, AsyncLocker locker2, AsyncLocker locker3)
    {
        if (locker1._id > locker3._id)
        {
            if (locker2._id > locker3._id)
            {
                await locker3._semaphore.WaitAsync();
                await locker1._semaphore.WaitAsync();
                await locker2._semaphore.WaitAsync();

                return new ReleaserThree(locker3._semaphore, locker1._semaphore, locker2._semaphore);
            }

            if (locker2._id > locker1._id)
            {
                await locker3._semaphore.WaitAsync();
                await locker2._semaphore.WaitAsync();
                await locker1._semaphore.WaitAsync();

                return new ReleaserThree(locker3._semaphore, locker2._semaphore, locker1._semaphore);
            }

            await locker3._semaphore.WaitAsync();
            await locker1._semaphore.WaitAsync();
            await locker2._semaphore.WaitAsync();

            return new ReleaserThree(locker3._semaphore, locker1._semaphore, locker2._semaphore);
        }

        if (locker1._id > locker2._id)
        {
            if (locker2._id > locker3._id)
            {
                await locker1._semaphore.WaitAsync();
                await locker3._semaphore.WaitAsync();
                await locker2._semaphore.WaitAsync();

                return new ReleaserThree(locker1._semaphore, locker3._semaphore, locker2._semaphore);
            }

            await locker1._semaphore.WaitAsync();
            await locker2._semaphore.WaitAsync();
            await locker3._semaphore.WaitAsync();

            return new ReleaserThree(locker1._semaphore, locker2._semaphore, locker3._semaphore);
        }

        if (locker2._id > locker3._id)
        {
            await locker2._semaphore.WaitAsync();
            await locker3._semaphore.WaitAsync();
            await locker1._semaphore.WaitAsync();

            return new ReleaserThree(locker2._semaphore, locker3._semaphore, locker1._semaphore);
        }

        await locker1._semaphore.WaitAsync();
        await locker2._semaphore.WaitAsync();
        await locker3._semaphore.WaitAsync();

        return new ReleaserThree(locker1._semaphore, locker2._semaphore, locker3._semaphore);
    }

    public bool IsLocked => _semaphore.CurrentCount == 0;

    public void Dispose()
    {
        if (_isDisposed)
            return;
        _isDisposed = true;
        if (!IsLocked)
            _semaphore.Dispose();
    }

    private class Releaser : IDisposable
    {
        private readonly SemaphoreSlim _semaphore;

        public Releaser(SemaphoreSlim semaphore)
        {
            _semaphore = semaphore;
        }

        public void Dispose()
        {
            _semaphore.Release();
        }
    }

    private class ReleaserTwo : IDisposable
    {
        private readonly SemaphoreSlim _semaphore1;
        private readonly SemaphoreSlim _semaphore2;

        public ReleaserTwo(SemaphoreSlim semaphore1, SemaphoreSlim semaphore2)
        {
            _semaphore1 = semaphore1;
            _semaphore2 = semaphore2;
        }

        public void Dispose()
        {
            _semaphore1.Release();
            _semaphore2.Release();
        }
    }

    private class ReleaserThree : IDisposable
    {
        private readonly SemaphoreSlim _semaphore1;
        private readonly SemaphoreSlim _semaphore2;
        private readonly SemaphoreSlim _semaphore3;

        public ReleaserThree(SemaphoreSlim semaphore1, SemaphoreSlim semaphore2, SemaphoreSlim semaphore3)
        {
            _semaphore1 = semaphore1;
            _semaphore2 = semaphore2;
            _semaphore3 = semaphore3;
        }

        public void Dispose()
        {
            _semaphore1.Release();
            _semaphore2.Release();
            _semaphore3.Release();
        }
    }

    private class ReleaserMultiple : IDisposable
    {
        private List<AsyncLocker> _lockers;

        public ReleaserMultiple(List<AsyncLocker> lockers)
        {
            _lockers = lockers;
        }

        public void Dispose()
        {
            foreach (var locker in _lockers)
            {
                locker._semaphore.Release();
            }
        }
    }
}