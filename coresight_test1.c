int coresight_test1(int val)
{
    int i;
    for (i = 0; i < 5; i++)
        val += 2;
    return val;
}